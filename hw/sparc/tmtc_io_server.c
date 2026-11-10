#include "qemu/osdep.h"
#include "tmtc_io_server.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "trace.h"

static const char stix_control_token[] = "STIX_CONTROL";

#define TOKEN_LEN (sizeof(stix_control_token) - 1)

/* bytes needed before a unit's CCSDS length field is on the wire */
#define TC_HDR_LEN 10u

static void stix_tmtc_accept(void *opaque);
static void stix_tmtc_client_read(void *opaque);
static void stix_tmtc_client_write(void *opaque);
static void stix_tmtc_udp_tc_read(void *opaque);
static void stix_tmtc_udp_close(StixTmtc *t);

/*
 * Close the client once (the baseline closed it twice), drop the queue
 * and re-arm the accept for the next one.
 */
static void stix_tmtc_client_close(StixTmtc *t)
{
    qemu_set_fd_handler(t->client_fd, NULL, NULL, NULL);
    close(t->client_fd);
    t->client_fd = -1;
    t->out_len = 0;
    t->out_sent = 0;
    t->token_len = 0;
    t->token_done = false;
    t->unit_len = 0;
    t->unit_need = 0;
    if (t->listen_fd >= 0) {
        /* re-arm the accept for the next client */
        qemu_set_fd_handler(t->listen_fd, stix_tmtc_accept, NULL, t);
    }
    trace_stixtmtc_client_disconnected();
}

/*
 * Write as much of the queue as the socket takes. The queue head is
 * never abandoned: a partial write keeps its remainder and is finished
 * before anything else, so no half packet ever reaches the wire.
 */
static void stix_tmtc_flush(StixTmtc *t)
{
    while (t->out_sent < t->out_len) {
        ssize_t n = send(t->client_fd, t->out_buf + t->out_sent,
                         t->out_len - t->out_sent, 0);

        if (n > 0) {
            t->out_sent += n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* the writable handler finishes the queue */
            return;
        }
        stix_tmtc_client_close(t);
        return;
    }

    /* drained: drop the write interest (level-triggered fd handlers) */
    t->out_sent = 0;
    t->out_len = 0;
    qemu_set_fd_handler(t->client_fd, stix_tmtc_client_read, NULL, t);
}

static void stix_tmtc_client_write(void *opaque)
{
    stix_tmtc_flush(opaque);
}

/*
 * D-K framing: a unit is [4-byte SpW header][CCSDS Space Packet], back
 * to back; the unit length is 4 + 7 + BE16(unit[8:10]) (the CCSDS
 * packet length field at SP[4:6]). Byte 1 of the header must be 0x02;
 * anything else drops one byte, counts a resync and retries. Units are
 * reassembled across recv chunks and delivered whole, never before the
 * full length has arrived.
 */
static void stix_tmtc_frame(StixTmtc *t, const uint8_t *data, size_t len)
{
    const uint8_t *p = data;

    /* the cap covers the largest unit a 16-bit packet length can give */
    QEMU_BUILD_BUG_ON(11 + 0xffff > TC_UNIT_MAX);

    while (len > 0) {
        size_t take;

        if (t->unit_len < TC_HDR_LEN) {
            take = MIN((size_t)(TC_HDR_LEN - t->unit_len), len);
            memcpy(t->unit_buf + t->unit_len, p, take);
            t->unit_len += take;
            p += take;
            len -= take;
            if (t->unit_len < TC_HDR_LEN) {
                return; /* the length field is not on the wire yet */
            }
            if (t->unit_buf[1] != 0x02) {
                memmove(t->unit_buf, t->unit_buf + 1, t->unit_len - 1);
                t->unit_len--;
                t->tc_resync++;
                trace_stixtmtc_resync();
                continue;
            }
            /*
             * Plausibility (P2-6 review m4): the FSW rejects anything
             * that is not CCSDS version 0, type TC, with the DFH flag
             * (FSW-RX §3.2 checks 2-4), so a head failing this is
             * garbage — resync instead of locking onto a bogus length
             * and swallowing the real TCs behind it.
             */
            if ((t->unit_buf[4] & 0xe0u) != 0 ||
                (t->unit_buf[4] & 0x18u) != 0x18u) {
                memmove(t->unit_buf, t->unit_buf + 1, t->unit_len - 1);
                t->unit_len--;
                t->tc_resync++;
                trace_stixtmtc_resync();
                continue;
            }
            t->unit_need = 11u + ((t->unit_buf[8] << 8) | t->unit_buf[9]);
        }
        take = MIN(t->unit_need - t->unit_len, len);
        memcpy(t->unit_buf + t->unit_len, p, take);
        t->unit_len += take;
        p += take;
        len -= take;
        if (t->unit_len == t->unit_need) {
            trace_stixtmtc_tc_unit((uint32_t)t->unit_len);
            if (t->tc_received) {
                t->tc_received(t->unit_buf, (int)t->unit_len, t->opaque);
            }
            t->unit_len = 0;
            t->unit_need = 0;
        }
    }
}

static void stix_tmtc_client_read(void *opaque)
{
    StixTmtc *t = opaque;
    uint8_t *p = t->in_buf;
    ssize_t n;

    do {
        n = recv(t->client_fd, t->in_buf, IO_TCP_SERVER_INPUT_BUFFER_SIZE, 0);
    } while (n < 0 && errno == EINTR);
    if (n == 0) {
        /* graceful close */
        stix_tmtc_client_close(t);
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        trace_stixtmtc_recv_error(errno);
        stix_tmtc_client_close(t);
        return;
    }

    if (!t->token_done) {
        /*
         * D-K: strip the handshake when it forms the first 12 bytes of
         * a connection. Accumulate the stream head until it can be
         * decided; anything else is TC data and goes to the framer.
         */
        size_t need = TOKEN_LEN - t->token_len;
        size_t take = MIN(need, (size_t)n);

        memcpy(t->token_buf + t->token_len, p, take);
        t->token_len += take;
        p += take;
        n -= take;

        if (t->token_len == TOKEN_LEN) {
            if (memcmp(t->token_buf, stix_control_token, TOKEN_LEN) == 0) {
                trace_stixtmtc_token_stripped();
            } else {
                stix_tmtc_frame(t, t->token_buf, TOKEN_LEN);
            }
            t->token_len = 0;
            t->token_done = true;
        }
    }

    if (n > 0) {
        /* TC units are reassembled here and delivered whole (BQL held) */
        stix_tmtc_frame(t, p, (size_t)n);
    }
}

static void stix_tmtc_accept(void *opaque)
{
    StixTmtc *t = opaque;
    struct sockaddr_in sin;
    socklen_t addrlen = sizeof(sin);
    Error *local_err = NULL;
    int fd;

    assert(t->client_fd == -1);

    fd = qemu_accept(t->listen_fd, (struct sockaddr *)&sin, &addrlen);
    if (fd < 0) {
        trace_stixtmtc_accept_failed();
        return;
    }
    if (!qemu_set_blocking(fd, false, &local_err)) {
        error_reportf_err(local_err, "tmtc: ");
        close(fd);
        return;
    }

    /*
     * One client at a time: drop the accept interest until this one
     * disconnects. QEMU fd handlers are level-triggered, so an always
     * armed handler would return here on every main-loop pass while a
     * second connection waits in the backlog and busy-spin the loop.
     */
    qemu_set_fd_handler(t->listen_fd, NULL, NULL, NULL);

    t->client_fd = fd;
    t->token_len = 0;
    t->token_done = false;
    t->unit_len = 0;
    t->unit_need = 0;
    qemu_set_fd_handler(t->client_fd, stix_tmtc_client_read,
                        stix_tmtc_client_write, t);
    trace_stixtmtc_client_connected(inet_ntoa(sin.sin_addr),
                                    ntohs(sin.sin_port));
}

void stix_tmtc_send(StixTmtc *t, const uint8_t *data, int len)
{
    trace_stixtmtc_tm_send();

    if (t->client_fd < 0) {
        /* no client: drop silently, not counted in tm-dropped */
        trace_stixtmtc_socket_closed();
        return;
    }

    if (t->out_sent > 0 &&
        TMTC_OUT_QUEUE_SIZE - t->out_len < (size_t)len) {
        /* compact the drained prefix so the packet can fit whole */
        memmove(t->out_buf, t->out_buf + t->out_sent,
                t->out_len - t->out_sent);
        t->out_len -= t->out_sent;
        t->out_sent = 0;
    }

    if ((size_t)len > TMTC_OUT_QUEUE_SIZE - t->out_len) {
        /* all-or-nothing: drop the packet whole, never split it */
        t->tm_dropped++;
        trace_stixtmtc_tm_dropped((uint32_t)len);
        return;
    }

    memcpy(t->out_buf + t->out_len, data, len);
    t->out_len += len;

    stix_tmtc_flush(t);
    if (t->out_sent < t->out_len) {
        /* a remainder is left: the writable handler finishes it */
        qemu_set_fd_handler(t->client_fd, stix_tmtc_client_read,
                            stix_tmtc_client_write, t);
    }
}

void stix_tmtc_cleanup(StixTmtc *t)
{
    if (t->client_fd >= 0) {
        qemu_set_fd_handler(t->client_fd, NULL, NULL, NULL);
        close(t->client_fd);
        t->client_fd = -1;
    }
    if (t->listen_fd >= 0) {
        qemu_set_fd_handler(t->listen_fd, NULL, NULL, NULL);
        close(t->listen_fd);
        t->listen_fd = -1;
    }
    stix_tmtc_udp_close(t);
    g_free(t->in_buf);
    t->in_buf = NULL;
    g_free(t->out_buf);
    t->out_buf = NULL;
    g_free(t->unit_buf);
    t->unit_buf = NULL;
    t->unit_len = 0;
    t->unit_need = 0;
    t->out_len = 0;
    t->out_sent = 0;
}

/*
 * Start the TMTC TCP server. Everything runs on main-loop fd handlers
 * from here on; there is no thread. Returns true on success; on
 * failure sets @errp (the bind error carries the port and strerror)
 * and returns false.
 */
bool init_tmtc_server(StixTmtc *tmtc, StixTmtcRecvFn tc_received, void *opaque,
                      uint32_t tmtc_port, Error **errp)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        /*
         * security fix: bind loopback (the 2022 code computed the
         * address but never used it). A remote ground station later
         * will want a bind-address property instead.
         */
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };
    int port;
    int optval = 1;

    tmtc->tc_received = tc_received;
    tmtc->opaque = opaque;
    tmtc->listen_fd = -1;
    tmtc->client_fd = -1;
    tmtc->token_len = 0;
    tmtc->token_done = false;
    tmtc->unit_len = 0;
    tmtc->unit_need = 0;
    tmtc->tc_resync = 0;
    tmtc->out_len = 0;
    tmtc->out_sent = 0;
    tmtc->tm_dropped = 0;

    /* Make unique port number */
    port = tmtc_port != 0 ? (int)tmtc_port
                          : IO_TCP_SERVER_BASE_PORT + getuid();

    tmtc->listen_fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (tmtc->listen_fd < 0) {
        error_setg_errno(errp, errno,
                         "TMTC server: cannot create the listening socket (port %d)",
                         port);
        return false;
    }

    sin.sin_port = htons(port);
    setsockopt(tmtc->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               &optval, sizeof(optval));

    if (bind(tmtc->listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        int err = errno;

        close(tmtc->listen_fd);
        tmtc->listen_fd = -1;
        error_setg_errno(errp, err,
                         "TMTC server: cannot bind 127.0.0.1:%d", port);
        return false;
    }
    if (listen(tmtc->listen_fd, 1) < 0) {
        int err = errno;

        close(tmtc->listen_fd);
        tmtc->listen_fd = -1;
        error_setg_errno(errp, err,
                         "TMTC server: cannot listen on 127.0.0.1:%d", port);
        return false;
    }
    if (!qemu_set_blocking(tmtc->listen_fd, false, errp)) {
        close(tmtc->listen_fd);
        tmtc->listen_fd = -1;
        return false;
    }

    tmtc->in_buf = g_malloc(IO_TCP_SERVER_INPUT_BUFFER_SIZE);
    tmtc->out_buf = g_malloc(TMTC_OUT_QUEUE_SIZE);
    tmtc->unit_buf = g_malloc(TC_UNIT_MAX);

    qemu_set_fd_handler(tmtc->listen_fd, stix_tmtc_accept, NULL, tmtc);
    trace_stixtmtc_listening(port);

    return true;
}

/*
 * One wake-up drains at most this many datagrams. The RX no-spill queue
 * (SPW_RX_QUEUE_LEN in grlib_stixspw.c) holds 64 units, so past 64 every
 * further datagram is only rx-dropped; and draining the whole socket
 * buffer in one go would hold the BQL for hundreds of deliveries, where
 * the TCP side reads one 8 KiB chunk per wake-up. The level-triggered
 * handler re-fires while datagrams remain (P2-8 review m4).
 */
#define UDP_DRAIN_MAX 64

/*
 * D-B TC ingress over UDP: each datagram is exactly one unit. The
 * datagram is read after room for the 4-byte SpW header, so the
 * default (stripped) mode only prepends it in place; with
 * udp-spw-header=on the datagram already starts with its own header
 * and is delivered as received. A datagram smaller than a bare Space
 * Packet (7 bytes; 4 + 7 with the header kept, the TCP framer's
 * minimum) or bigger than the largest possible unit is dropped and
 * traced; there is no resync logic, the datagram boundaries are the
 * framing.
 */
static void stix_tmtc_udp_tc_read(void *opaque)
{
    StixTmtc *t = opaque;
    static const uint8_t spw_hdr[4] = { 0x40, 0x02, 0x00, 0x00 };
    struct sockaddr_in sin;
    /*
     * the largest accepted datagram plus one, so an oversize one shows
     * up as a short read instead of a silent truncation; in the default
     * (stripped) mode the datagram is a bare Space Packet, which can
     * never exceed TC_UNIT_MAX - 4 (16-bit CCSDS length)
     */
    const size_t max = (t->udp_spw_header ? TC_UNIT_MAX
                                          : TC_UNIT_MAX - 4) + 1;
    const size_t min = t->udp_spw_header ? 4u + 7u : 7u;
    uint8_t *unit = t->udp_buf;
    int i;
    ssize_t n;

    for (i = 0; i < UDP_DRAIN_MAX; i++) {
        socklen_t addrlen = sizeof(sin);

        do {
            n = recvfrom(t->udp_tc_fd, t->udp_buf + 4, max, 0,
                         (struct sockaddr *)&sin, &addrlen);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                trace_stixtmtc_udp_tc_error(errno);
            }
            return;
        }
        if (n < min || (size_t)n > max - 1) {
            trace_stixtmtc_udp_tc_dropped((int)n);
            continue;
        }
        if (!t->udp_spw_header) {
            memcpy(t->udp_buf, spw_hdr, sizeof(spw_hdr));
        } else {
            /* the datagram carries its own header: deliver as received */
            unit = t->udp_buf + 4;
        }
        trace_stixtmtc_udp_tc_unit(t->udp_spw_header ? (int)n : (int)(n + 4),
                                    ntohs(sin.sin_port));
        if (t->tc_received) {
            t->tc_received(unit, t->udp_spw_header ? (int)n : (int)(n + 4),
                           t->opaque);
        }
    }
}

/*
 * Close the UDP sockets opened so far. The TCP server state (listen
 * fd, client, queue) is untouched: a UDP failure must not tear it down.
 */
static void stix_tmtc_udp_close(StixTmtc *t)
{
    if (t->udp_tc_fd >= 0) {
        qemu_set_fd_handler(t->udp_tc_fd, NULL, NULL, NULL);
        close(t->udp_tc_fd);
        t->udp_tc_fd = -1;
    }
    if (t->udp_tm_fd >= 0) {
        close(t->udp_tm_fd);
        t->udp_tm_fd = -1;
    }
    g_free(t->udp_buf);
    t->udp_buf = NULL;
}

/*
 * Open the D-B UDP sockets. The TM socket is send-only towards one
 * destination (no bind, unconnected); the TC socket binds the loopback
 * port and is served by a main-loop fd handler. Both are non-blocking
 * and unconnected (any TC source port is accepted).
 */
bool stix_tmtc_udp_open(StixTmtc *t, StixTmtcRecvFn tc_received,
                        void *opaque, const char *udp_tm_host,
                        uint32_t udp_tm_port, uint32_t udp_tc_port,
                        bool udp_spw_header, Error **errp)
{
    /*
     * set unconditionally: when the TCP server is off (tmtc-server=off,
     * or auto under qtest) this is the only place the RX callback is
     * wired
     */
    t->tc_received = tc_received;
    t->opaque = opaque;
    t->udp_spw_header = udp_spw_header;

    if (udp_tm_port > 0xffff) {
        error_setg(errp, "TMTC UDP: udp-tm-port %u is out of range",
                   udp_tm_port);
        return false;
    }
    if (udp_tc_port > 0xffff) {
        error_setg(errp, "TMTC UDP: udp-tc-port %u is out of range",
                   udp_tc_port);
        return false;
    }

    if (udp_tm_port != 0) {
        struct sockaddr_in *d = &t->udp_tm_dest;

        t->udp_tm_fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
        if (t->udp_tm_fd < 0) {
            error_setg_errno(errp, errno,
                             "TMTC UDP: cannot create the TM socket");
            return false;
        }
        if (!qemu_set_blocking(t->udp_tm_fd, false, errp)) {
            stix_tmtc_udp_close(t);
            return false;
        }
        memset(d, 0, sizeof(*d));
        d->sin_family = AF_INET;
        /*
         * numeric IPv4 only: no name resolution on the realize path
         * (the default and documented value is 127.0.0.1)
         */
        if (inet_pton(AF_INET, udp_tm_host, &d->sin_addr) != 1) {
            error_setg(errp, "TMTC UDP: udp-tm-host '%s' is not an"
                             " IPv4 address", udp_tm_host);
            stix_tmtc_udp_close(t);
            return false;
        }
        d->sin_port = htons((uint16_t)udp_tm_port);
    }

    if (udp_tc_port != 0) {
        struct sockaddr_in sin = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = inet_addr("127.0.0.1"),
        };
        int port = (int)udp_tc_port;

        t->udp_tc_fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
        if (t->udp_tc_fd < 0) {
            error_setg_errno(errp, errno,
                             "TMTC UDP: cannot create the TC socket"
                             " (port %d)", port);
            stix_tmtc_udp_close(t);
            return false;
        }
        sin.sin_port = htons((uint16_t)port);
        if (bind(t->udp_tc_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            int err = errno;

            error_setg_errno(errp, err,
                             "TMTC UDP: cannot bind 127.0.0.1:%d", port);
            stix_tmtc_udp_close(t);
            return false;
        }
        if (!qemu_set_blocking(t->udp_tc_fd, false, errp)) {
            stix_tmtc_udp_close(t);
            return false;
        }
        /* the datagram scratch: hdr room + the largest datagram + 1 */
        t->udp_buf = g_malloc(4 + TC_UNIT_MAX + 1);
        qemu_set_fd_handler(t->udp_tc_fd, stix_tmtc_udp_tc_read, NULL, t);
        trace_stixtmtc_udp_tc_bind(port);
    }

    return true;
}

void stix_tmtc_udp_send(StixTmtc *t, const uint8_t *data, int len)
{
    /*
     * the datagram length: the DMA'd buffer minus the stripped header,
     * so every UDP TM trace logs the same length (review m5)
     */
    int dlen;
    ssize_t n;

    if (t->udp_tm_fd < 0 || len <= 0) {
        return;
    }
    if (t->udp_spw_header) {
        dlen = len;
        n = sendto(t->udp_tm_fd, data, len, 0,
                   (struct sockaddr *)&t->udp_tm_dest,
                   sizeof(t->udp_tm_dest));
    } else {
        if (len < 4) {
            /* nothing to strip: not a SpW packet; a send failure */
            t->tm_dropped++;
            trace_stixtmtc_udp_tm_short(len);
            return;
        }
        dlen = len - 4;
        n = sendto(t->udp_tm_fd, data + 4, dlen, 0,
                   (struct sockaddr *)&t->udp_tm_dest,
                   sizeof(t->udp_tm_dest));
    }
    if (n < 0) {
        /*
         * a whole datagram failed (UDP has no partial sends): dropped
         * and counted, like an out-queue overflow. An unconnected
         * socket never sees ICMP feedback, so a TX with nothing
         * listening is NOT a failure.
         */
        t->tm_dropped++;
        trace_stixtmtc_udp_tm_dropped(dlen, errno);
        return;
    }
    trace_stixtmtc_udp_tm_send(dlen);
}
