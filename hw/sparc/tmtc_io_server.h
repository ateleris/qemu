#ifndef __IOTCPSERVER_H__
#define __IOTCPSERVER_H__

#include "qapi/error.h"
#include "qemu/sockets.h" /* struct sockaddr_in */

#define IO_TCP_SERVER_INPUT_BUFFER_SIZE 8192
#define IO_TCP_SERVER_BASE_PORT		20000

/* TM out-queue bound: packets that do not fit whole are dropped */
#define TMTC_OUT_QUEUE_SIZE (256u * 1024)

/*
 * D-K TC framing: a unit is [4-byte SpW header][CCSDS Space Packet],
 * its length 4 + 7 + BE16(unit[8:10]). A 16-bit packet length makes
 * 4 + 65542 the largest possible unit.
 */
#define TC_UNIT_MAX (4u + 65542u)

/*
 * Called with one framed TC unit in the main loop (BQL held).
 */
typedef void (*StixTmtcRecvFn)(uint8_t *data, int length, void *opaque);

/*
 * Transport state, owned by the device (D-N): one listening socket, one
 * client, everything driven by main-loop fd handlers. All fds are -1
 * while the server is off.
 */
struct StixTmtc {
    StixTmtcRecvFn tc_received;
    void *opaque;

    int listen_fd; /* -1 when the server is off */
    int client_fd; /* -1 when no client is connected */

    uint8_t *in_buf;       /* per-connection receive buffer */
    uint8_t token_buf[12]; /* stream head, for the STIX_CONTROL strip */
    size_t token_len;      /* stream-head bytes seen so far */
    bool token_done;       /* the strip was decided for this connection */

    uint8_t *unit_buf; /* D-K framing buffer, TC_UNIT_MAX bytes */
    size_t unit_len;   /* bytes accumulated for the pending unit */
    size_t unit_need;  /* 0 until the CCSDS length is known */
    uint32_t tc_resync; /* framing resyncs (one dropped byte each) */

    uint8_t *out_buf; /* whole-packet TM FIFO, TMTC_OUT_QUEUE_SIZE bytes */
    size_t out_len;   /* bytes queued */
    size_t out_sent;  /* bytes of the queue already on the wire */

    /*
     * per-sink drops (P2-8 review m5): the TCP queue-full drop and a
     * UDP sendto failure both count here, so one TX can count once per
     * failing sink — or once when only one sink failed (see
     * accepted-diffs §21)
     */
    uint32_t tm_dropped;

    /*
     * D-B direct UDP for the ground station (P2-8). Both sockets are
     * plain datagram sockets and NOT connected: the TC side must accept
     * datagrams from any source port (the GS sends from an ephemeral
     * one, and char-udp's connected socket would drop them). Both fds
     * are -1 while the respective port is 0.
     */
    int udp_tm_fd; /* send-only, no bind; -1 when udp-tm-port = 0 */
    int udp_tc_fd; /* bound, fd handler; -1 when udp-tc-port = 0 */
    struct sockaddr_in udp_tm_dest; /* TM destination, resolved at open */
    bool udp_spw_header; /* keep the 4-byte SpW header in both directions */
    uint8_t *udp_buf; /* datagram scratch, 4 + TC_UNIT_MAX + 1 bytes */
};
typedef struct StixTmtc StixTmtc;

/*
 * Start the TMTC TCP server. Returns true on success; on failure sets
 * @errp (the bind error carries the port and strerror) and returns
 * false.
 */
bool init_tmtc_server(StixTmtc *tmtc, StixTmtcRecvFn tc_received, void *opaque,
                      uint32_t tmtc_port, Error **errp);

/*
 * Queue one TM packet for the connected client, all-or-nothing: the
 * stream carries one D-K unit ([4-byte SpW header][CCSDS packet]) per
 * packet, back to back, so a packet that does not fit the out-queue is
 * dropped whole (tm-dropped), and a partial write keeps its remainder
 * at the queue head. With no client the packet is dropped silently
 * (not counted in tm-dropped). Never blocks.
 */
void stix_tmtc_send(StixTmtc *tmtc, const uint8_t *data, int len);

/*
 * Open the D-B UDP sockets (P2-8), independent of the TCP server. The
 * TM socket is send-only towards udp_tm_host:udp_tm_port (no bind); the
 * TC socket binds 127.0.0.1:udp_tc_port and gets a main-loop fd handler
 * delivering one unit per datagram through @tc_received (the same
 * callback init_tmtc_server takes, set here too when the TCP server is
 * off). Returns true on success; on failure sets @errp (a bind error
 * carries the port and strerror) and returns false with any socket
 * already opened closed.
 */
bool stix_tmtc_udp_open(StixTmtc *tmtc, StixTmtcRecvFn tc_received,
                        void *opaque, const char *udp_tm_host,
                        uint32_t udp_tm_port, uint32_t udp_tc_port,
                        bool udp_spw_header, Error **errp);

/*
 * Send one TM packet out as one UDP datagram to udp-tm-host:udp-tm-port,
 * the 4-byte SpW header stripped unless udp-spw-header is on. Never
 * blocks; a sendto failure counts in tm-dropped (per-sink accounting —
 * an unconnected socket never sees ICMP port-unreachable feedback, so a
 * dead destination is not a failure here; see accepted-diffs §21).
 */
void stix_tmtc_udp_send(StixTmtc *tmtc, const uint8_t *data, int len);

/*
 * Close the sockets and free the buffers. Safe after instance_init,
 * which sets all fds to -1 (a zeroed StixTmtc would have
 * client_fd == 0 and would close stdin).
 */
void stix_tmtc_cleanup(StixTmtc *tmtc);

#endif
