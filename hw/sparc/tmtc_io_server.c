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
