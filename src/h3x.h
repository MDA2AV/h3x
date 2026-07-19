/*
 * h3x - an HTTP/3 load generator built on h2o's client stack.
 *
 * All of the hard parts - the QUIC transport (quicly), TLS (picotls), HTTP/3
 * framing + QPACK, and the batched UDP I/O - are reused from libh2o-evloop. This
 * program only adds what a load generator needs on top of h2o's src/httpclient.c:
 * shared-nothing worker threads, a closed-loop concurrency driver, connection
 * churn with in-memory session resumption (0-RTT), and merged latency percentiles.
 * The per-connection setup mirrors h2o's httpclient.c.
 *
 * Layout: main.c (CLI parsing + thread lifecycle), worker.c (per-thread QUIC/TLS
 * context and the event loop), request.c (request lifecycle + client callbacks),
 * tls.c (session resumption callbacks), stats.c (latency samples + run summary).
 */
#ifndef H3X_H
#define H3X_H

#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include "picotls.h"
#include "quicly.h"
#include "h2o/httpclient.h"

#define DEFAULT_IO_TIMEOUT 5000

/* Read-only after main() finishes parsing; shared by every worker thread. */
struct config {
    const char *url;
    const char *connect_to; /* -x : override the layer-4 target (skip DNS / pin a backend) */
    const char *method;
    struct {
        h2o_iovec_t name;
        h2o_iovec_t value;
    } headers[256];
    size_t num_headers;
    unsigned total_requests; /* -n : total across all threads */
    unsigned concurrency;    /* -c : concurrent streams per connection */
    unsigned send_batch;     /* accumulate this many freed slots before refilling, so requests
                                queue together and quicly packs them into fewer datagrams (1 = off) */
    unsigned threads;        /* -t : worker threads (0 = one per CPU) */
    unsigned connections;    /* --connections : total connections across all threads (0 = one per thread) */
    double duration;         /* -d : run for this many seconds instead of a fixed -n count */
    unsigned reconnect;      /* --reconnect N : requests per connection (0 = one per worker) */
    int verify_none;         /* -k */
    int no_resumption;       /* --no-resumption : force a full handshake on every connection */
    uint64_t recv_window;    /* -W */
    uint64_t io_timeout;
    /* QUIC/TLS transport tuning - the experiment variables of a benchmark */
    uint64_t max_udp_payload_size;     /* --max-udp-payload-size */
    uint16_t initial_udp_payload_size; /* --initial-udp-payload-size */
    int disallow_delayed_ack;          /* --disallow-delayed-ack */
    int have_ack_frequency;            /* --ack-frequency given */
    double ack_frequency;
    int no_ecn;               /* --no-ecn */
    uint32_t qpack_table;     /* --qpack-table (encoder dynamic table capacity) */
    const char *key_exchange; /* --key-exchange <name> */
};
extern struct config conf;

extern const ptls_key_exchange_algorithm_t *h3_key_exchanges[8];

/* In-memory resumption state, one per worker. The first connection does a full
 * handshake and the server hands back a ticket; later connections present it and
 * attempt 0-RTT. This replaces httpclient.c's file-based `-s`, which is the wrong
 * shape for a load generator. */
struct session_cache {
    ptls_iovec_t ticket; /* TLS session ticket */
    ptls_iovec_t token;  /* QUIC address token (NEW_TOKEN) */
    quicly_transport_parameters_t tp;
    int have_ticket;
};

/* One per thread. Everything here is touched by a single thread only, so no locking. */
struct worker {
    pthread_t tid;
    unsigned idx;
    unsigned req_target;   /* per-worker request target (count mode; unused with -d) */
    unsigned started;      /* requests started so far */
    unsigned conn_started; /* requests started on the current connection */
    unsigned inflight;
    h2o_loop_t *loop;
    h2o_httpclient_ctx_t ctx;
    h2o_http3client_ctx_t h3ctx;
    quicly_cid_plaintext_t next_cid;
    h2o_multithread_queue_t *queue;
    h2o_multithread_receiver_t getaddr_receiver;
    h2o_httpclient_connection_pool_t *connpools; /* one per connection this worker drives */
    h2o_socketpool_t sockpool;                   /* one shared by all this worker's connpools */
    unsigned n_conns; /* number of connections (pools) this worker drives */
    unsigned rr;      /* round-robin index for dispatching the next request to a connection */
    unsigned active_conns; /* connpools currently in rotation; ramps up from CONN_RAMP_INITIAL */
    unsigned established;  /* connpools whose first request has completed (paces the ramp) */
    uint8_t *conn_up;      /* per-connpool: 1 once its connection's first request completed */
    h2o_url_t target;
    h2o_url_t connect_to;
    /* resumption */
    ptls_save_ticket_t save_ticket;
    quicly_save_resumption_token_t save_token;
    struct session_cache sess;
    /* stats */
    uint32_t *lat_us;
    size_t lat_n, lat_cap;
    uint64_t body_bytes;
    unsigned n_ok, n_fail, n_resumed, conn_count;
};

/* run budget: a fixed request count, or a wall-clock duration via -d.
 * g_deadline is the absolute stop time; set in main when -d is used. */
extern struct timeval g_deadline;

/* worker.c */
void *worker_main(void *arg);

/* request.c */
void start_one(struct worker *w);
int may_start(struct worker *w);
int overall_has_more(struct worker *w);
void close_and_drain(struct worker *w);

/* tls.c */
int load_session_cb(h2o_httpclient_ctx_t *ctx, struct sockaddr *server_addr, const char *server_name,
                    ptls_iovec_t *address_token, ptls_iovec_t *session_ticket, quicly_transport_parameters_t *tp);
int save_ticket_cb(ptls_save_ticket_t *self, ptls_t *tls, ptls_iovec_t src);
quicly_error_t save_token_cb(quicly_save_resumption_token_t *self, quicly_conn_t *conn, ptls_iovec_t token);

/* stats.c */
void record_latency(struct worker *w, const struct timeval *start);
unsigned print_summary(struct worker *workers, double elapsed);

#endif
