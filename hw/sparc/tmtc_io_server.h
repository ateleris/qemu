#ifndef __IOTCPSERVER_H__
#define __IOTCPSERVER_H__

#include "qapi/error.h"

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

    uint32_t tm_dropped; /* packets dropped whole (queue full) */
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
 * Close the sockets and free the buffers. Safe after instance_init,
 * which sets both fds to -1 (a zeroed StixTmtc would have
 * client_fd == 0 and would close stdin).
 */
void stix_tmtc_cleanup(StixTmtc *tmtc);

#endif
