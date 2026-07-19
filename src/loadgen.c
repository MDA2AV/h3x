/*
 * h3x - an HTTP/3 load generator built on h2o's client stack.
 *
 * All of the hard parts - the QUIC transport (quicly), TLS (picotls), HTTP/3
 * framing + QPACK, and the batched UDP I/O - are reused from libh2o-evloop. This
 * file only adds what a load generator needs on top of h2o's src/httpclient.c:
 * shared-nothing worker threads, a closed-loop concurrency driver, connection
 * churn with in-memory session resumption (0-RTT), and merged latency percentiles.
 * The per-connection setup mirrors h2o's httpclient.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* sched_getaffinity, to honor the container's CPU set */
#endif
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include "picotls.h"
#include "picotls/openssl.h"
#include "quicly.h"
#include "h2o/hostinfo.h"
#include "h2o/httpclient.h"

#define DEFAULT_IO_TIMEOUT 5000

/* Connection-establishment pacing. Opening every connection's handshake at once floods a worker's
 * event loop at high connection counts, and a chunk of the handshakes time out - the dominant
 * failure source in the many-connection regime. Instead open an initial set and grow it only as
 * connections come up, keeping the number of in-flight (not-yet-established) handshakes under a cap. */
#define CONN_RAMP_INITIAL 32   /* handshakes a worker starts immediately */
#define CONN_RAMP_STEP 8       /* connections added per loop pass while ramping */
#define CONN_MAX_HANDSHAKES 32 /* cap on concurrent in-flight handshakes per worker */

/* Read-only after main() finishes parsing; shared by every worker thread. */
static struct {
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
} conf = {
    .method = "GET",
    .total_requests = 100,
    .concurrency = 10,
    /* .threads = 0 means auto: one per available CPU (see detect_ncpu) */
    .threads = 0,
    .io_timeout = DEFAULT_IO_TIMEOUT,
    .qpack_table = 4096,
};

static const char *progname;
static const ptls_key_exchange_algorithm_t *h3_key_exchanges[8];

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

/* Per-request scratch, allocated in the request pool and stashed in client->data. */
struct req_ctx {
    struct worker *w;
    struct timeval start;
    unsigned conn_idx; /* connpool this request was dispatched to (for establishment tracking) */
};

static h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *method, h2o_url_t *url,
                                         const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                         h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                         h2o_url_t *origin);
static h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, h2o_httpclient_on_head_t *args);
static int on_body(h2o_httpclient_t *client, const char *errstr, h2o_header_t *trailers, size_t num_trailers);
static void start_one(struct worker *w);

static const char *ca_bundle(void)
{
    const char *e = getenv("H3X_CA_BUNDLE");
    return e != NULL ? e : "/etc/ssl/certs/ca-certificates.crt";
}

/* ---- resumption callbacks (wired unless --no-resumption) ---- */

static int load_session_cb(h2o_httpclient_ctx_t *ctx, struct sockaddr *server_addr, const char *server_name,
                           ptls_iovec_t *address_token, ptls_iovec_t *session_ticket, quicly_transport_parameters_t *tp)
{
    struct worker *w = H2O_STRUCT_FROM_MEMBER(struct worker, ctx, ctx);
    *address_token = ptls_iovec_init(NULL, 0);
    *session_ticket = ptls_iovec_init(NULL, 0);
    *tp = (quicly_transport_parameters_t){};
    if (w->sess.have_ticket) {
        /* hand out fresh copies; h2o frees these (address_token right after connect,
         * session_ticket at connection teardown) */
        *session_ticket = ptls_iovec_init(h2o_mem_alloc(w->sess.ticket.len), w->sess.ticket.len);
        memcpy(session_ticket->base, w->sess.ticket.base, w->sess.ticket.len);
        *tp = w->sess.tp;
        if (w->sess.token.len != 0) {
            *address_token = ptls_iovec_init(h2o_mem_alloc(w->sess.token.len), w->sess.token.len);
            memcpy(address_token->base, w->sess.token.base, w->sess.token.len);
        }
    }
    return 1;
}

static int save_ticket_cb(ptls_save_ticket_t *self, ptls_t *tls, ptls_iovec_t src)
{
    struct worker *w = H2O_STRUCT_FROM_MEMBER(struct worker, save_ticket, self);
    quicly_conn_t *conn = *ptls_get_data_ptr(tls);
    free(w->sess.ticket.base);
    w->sess.ticket.base = h2o_mem_alloc(src.len);
    memcpy(w->sess.ticket.base, src.base, src.len);
    w->sess.ticket.len = src.len;
    w->sess.tp = *quicly_get_remote_transport_parameters(conn);
    w->sess.have_ticket = 1;
    return 0;
}

static quicly_error_t save_token_cb(quicly_save_resumption_token_t *self, quicly_conn_t *conn, ptls_iovec_t token)
{
    struct worker *w = H2O_STRUCT_FROM_MEMBER(struct worker, save_token, self);
    free(w->sess.token.base);
    w->sess.token.base = h2o_mem_alloc(token.len);
    memcpy(w->sess.token.base, token.base, token.len);
    w->sess.token.len = token.len;
    return 0;
}

/* ---- run budget: a fixed request count, or a wall-clock duration via -d ---- */

static struct timeval g_deadline; /* absolute stop time; set in main when -d is used */

static int deadline_reached(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec > g_deadline.tv_sec ||
           (now.tv_sec == g_deadline.tv_sec && now.tv_usec >= g_deadline.tv_usec);
}

static int overall_has_more(struct worker *w)
{
    return conf.duration != 0 ? !deadline_reached() : w->started < w->req_target;
}

static int may_start(struct worker *w)
{
    if (!overall_has_more(w))
        return 0;
    if (conf.reconnect != 0 && w->conn_started >= conf.reconnect)
        return 0;
    return 1;
}

static void close_and_drain(struct worker *w)
{
    h2o_quic_close_all_connections(&w->h3ctx.h3);
    while (h2o_quic_num_connections(&w->h3ctx.h3) != 0) {
        h2o_evloop_run(w->loop, 100);
        h2o_quic_flush_datagrams(&w->h3ctx.h3);
    }
}

/* ---- request lifecycle ---- */

static void record_latency(struct worker *w, const struct timeval *start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t us = (uint64_t)(now.tv_sec - start->tv_sec) * 1000000 + (now.tv_usec - start->tv_usec);
    /* ponytail: keep every sample for exact percentiles (4 bytes each). Fine into the
     * millions; swap for an HdrHistogram if runs get large enough for the array to hurt. */
    if (w->lat_n == w->lat_cap) {
        w->lat_cap = w->lat_cap != 0 ? w->lat_cap * 2 : 1024;
        w->lat_us = h2o_mem_realloc(w->lat_us, w->lat_cap * sizeof(uint32_t));
    }
    w->lat_us[w->lat_n++] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* Retire the request: record its outcome, free its pool, decrement the in-flight
 * count. The worker loop (not this callback) starts the replacement, which keeps
 * request creation out of teardown and avoids reentrancy. Mirrors how h2o's
 * httpclient.c frees client->pool inside the terminal on_body call. */
static void request_done(h2o_httpclient_t *client, int ok)
{
    struct req_ctx *rc = client->data;
    struct worker *w = rc->w;
    if (ok) {
        record_latency(w, &rc->start);
        ++w->n_ok;
        if (!w->conn_up[rc->conn_idx]) { /* first success on this connpool: it is established */
            w->conn_up[rc->conn_idx] = 1;
            ++w->established;
        }
    } else {
        ++w->n_fail;
    }
    h2o_mem_clear_pool(client->pool);
    free(client->pool);
    --w->inflight;
}

static void start_one(struct worker *w)
{
    h2o_mem_pool_t *pool = h2o_mem_alloc(sizeof(*pool));
    h2o_mem_init_pool(pool);
    struct req_ctx *rc = h2o_mem_alloc_pool(pool, *rc, 1);
    rc->w = w;
    gettimeofday(&rc->start, NULL);

    if (w->conn_started == 0)
        ++w->conn_count; /* first request of a new connection episode */
    ++w->started;
    ++w->conn_started;
    ++w->inflight;
    /* The first request on a connection establishes it; the rest reuse it as streams. h2o's
     * connection pool handles reuse and re-establishment. Round-robin across the worker's pools so
     * requests spread evenly over its connections, each carrying ~concurrency streams. */
    rc->conn_idx = w->rr;
    h2o_httpclient_connection_pool_t *cp = &w->connpools[w->rr];
    w->rr = (w->rr + 1) % w->active_conns;
    h2o_httpclient_connect(NULL, pool, rc, &w->ctx, cp, &w->target, NULL, on_connect);
}

static h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *method, h2o_url_t *url,
                                         const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                         h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                         h2o_url_t *origin)
{
    struct req_ctx *rc = client->data;
    if (errstr != NULL) {
        request_done(client, 0);
        return NULL;
    }

    *method = h2o_iovec_init(conf.method, strlen(conf.method));
    *url = rc->w->target;
    h2o_headers_t headers_vec = {NULL};
    for (size_t i = 0; i != conf.num_headers; ++i)
        h2o_add_header_by_str(client->pool, &headers_vec, conf.headers[i].name.base, conf.headers[i].name.len, 1, NULL,
                              conf.headers[i].value.base, conf.headers[i].value.len);
    *headers = headers_vec.entries;
    *num_headers = headers_vec.size;
    *body = h2o_iovec_init(NULL, 0);
    *proceed_req_cb = NULL;
    return on_head;
}

static h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, h2o_httpclient_on_head_t *args)
{
    struct req_ctx *rc = client->data;

    if (errstr != NULL && errstr != h2o_httpclient_error_is_eos) {
        request_done(client, 0);
        return NULL;
    }

    /* record whether this connection's TLS handshake was resumed (PSK / 0-RTT) */
    h2o_httpclient_conn_properties_t props;
    client->get_conn_properties(client, &props);
    if (props.ssl.session_reused == 1)
        ++rc->w->n_resumed;

    int ok = 200 <= args->status && args->status < 400;
    /* A response with no body ends here; on_body is never called (see h2o's
     * handle_input_expect_headers), so retire the request now. */
    if (errstr == h2o_httpclient_error_is_eos) {
        request_done(client, ok);
        return NULL;
    }
    return on_body;
}

static int on_body(h2o_httpclient_t *client, const char *errstr, h2o_header_t *trailers, size_t num_trailers)
{
    struct req_ctx *rc = client->data;

    if (errstr != NULL && errstr != h2o_httpclient_error_is_eos) {
        request_done(client, 0);
        return -1;
    }

    rc->w->body_bytes += (*client->buf)->size;
    h2o_buffer_consume(&(*client->buf), (*client->buf)->size);

    if (errstr == h2o_httpclient_error_is_eos)
        request_done(client, 1);
    return 0;
}

static void setup_connections(struct worker *w);

/* Build the per-thread HTTP/3 (quicly + picotls) context: TLS, QUIC transport params and tuning
 * knobs, resumption/0-RTT, certificate verification, and the UDP socket(s). */
static void setup_h3_context(struct worker *w)
{
    w->h3ctx = (h2o_http3client_ctx_t){
        .tls =
            {
                .random_bytes = ptls_openssl_random_bytes,
                .get_time = &ptls_get_time,
                .key_exchanges = h3_key_exchanges,
                .cipher_suites = ptls_openssl_cipher_suites,
            },
        .qpack = {.encoder_table_capacity = conf.qpack_table},
        .max_frame_payload_size = 16384,
    };
    quicly_amend_ptls_context(&w->h3ctx.tls);
    w->h3ctx.quic = quicly_spec_context;
    w->h3ctx.quic.transport_params.max_streams_uni = 10;
    w->h3ctx.quic.tls = &w->h3ctx.tls;
    {
        uint8_t random_key[PTLS_SHA256_DIGEST_SIZE];
        w->h3ctx.tls.random_bytes(random_key, sizeof(random_key));
        w->h3ctx.quic.cid_encryptor = quicly_new_default_cid_encryptor(
            &ptls_openssl_quiclb, &ptls_openssl_aes128ecb, &ptls_openssl_sha256, ptls_iovec_init(random_key, sizeof(random_key)));
        ptls_clear_memory(random_key, sizeof(random_key));
    }
    w->h3ctx.quic.stream_open = &h2o_httpclient_http3_on_stream_open;

    /* transport tuning knobs */
    if (conf.recv_window != 0) {
        w->h3ctx.quic.transport_params.max_stream_data.uni = conf.recv_window;
        w->h3ctx.quic.transport_params.max_stream_data.bidi_local = conf.recv_window;
        w->h3ctx.quic.transport_params.max_stream_data.bidi_remote = conf.recv_window;
    }
    if (conf.max_udp_payload_size != 0)
        w->h3ctx.quic.transport_params.max_udp_payload_size = conf.max_udp_payload_size;
    if (conf.initial_udp_payload_size != 0)
        w->h3ctx.quic.initial_egress_max_udp_payload_size = conf.initial_udp_payload_size;
    if (conf.disallow_delayed_ack)
        w->h3ctx.quic.transport_params.min_ack_delay_usec = UINT64_MAX;
    if (conf.have_ack_frequency)
        w->h3ctx.quic.ack_frequency = (uint16_t)(conf.ack_frequency * 1024);
    if (conf.no_ecn)
        w->h3ctx.quic.enable_ratio.ecn = 0;

    /* resumption / 0-RTT */
    if (!conf.no_resumption) {
        w->save_ticket.cb = save_ticket_cb;
        w->h3ctx.tls.save_ticket = &w->save_ticket;
        w->save_token.cb = save_token_cb;
        w->h3ctx.quic.save_resumption_token = &w->save_token;
        w->h3ctx.load_session = load_session_cb;
    }

    if (!conf.verify_none) {
        X509_STORE *store = X509_STORE_new();
        if (store == NULL || X509_STORE_load_locations(store, ca_bundle(), NULL) != 1) {
            fprintf(stderr, "failed to load CA bundle: %s (set H3X_CA_BUNDLE or pass -k)\n", ca_bundle());
            exit(EXIT_FAILURE);
        }
        ptls_openssl_init_verify_certificate(&w->h3ctx.verify_cert, store);
        X509_STORE_free(store);
        w->h3ctx.tls.verify_certificate = &w->h3ctx.verify_cert.super;
    }

    /* per-thread CID space, so connection IDs across threads never collide */
    w->next_cid = (quicly_cid_plaintext_t){.thread_id = w->idx};
    h2o_socket_t *socks[2], **sp = socks;
    if ((*sp = h2o_quic_create_client_socket(w->loop, AF_INET)) != NULL)
        ++sp;
    if ((*sp = h2o_quic_create_client_socket(w->loop, AF_INET6)) != NULL)
        ++sp;
    if (sp == socks) {
        perror("failed to create UDP socket");
        exit(EXIT_FAILURE);
    }
    h2o_quic_init_context(&w->h3ctx.h3, w->loop, socks[0], sp > socks + 1 ? socks[1] : NULL, &w->h3ctx.quic, &w->next_cid, NULL,
                          h2o_httpclient_http3_notify_connection_update, 1 /* use_gso */, NULL);
    w->h3ctx.h3.batch_sends = 1; /* coalesce all connections' sends into one sendmmsg per loop pass */
}

static void worker_init(struct worker *w)
{
    w->loop = h2o_evloop_create();
    setup_h3_context(w);

    /* generic client context: timeouts, buffer size, protocol ratios, and the DNS receiver */
    w->ctx = (h2o_httpclient_ctx_t){
        .loop = w->loop,
        .getaddr_receiver = &w->getaddr_receiver,
        .io_timeout = conf.io_timeout,
        .connect_timeout = conf.io_timeout,
        .first_byte_timeout = conf.io_timeout,
        .keepalive_timeout = conf.io_timeout,
        .max_buffer_size = 128 * 1024,
        .http3 = &w->h3ctx,
    };
    w->ctx.protocol_selector.ratio.http3 = 100; /* H3 only */

    w->queue = h2o_multithread_create_queue(w->loop);
    h2o_multithread_register_receiver(w->queue, &w->getaddr_receiver, h2o_hostinfo_getaddr_receiver);

    setup_connections(w);
}

/* Parse the target URL and set up this worker's connection pools. One socketpool is shared by all
 * the worker's connpools; each connpool becomes one QUIC connection over the shared UDP socket. */
static void setup_connections(struct worker *w)
{
    if (h2o_url_parse(NULL, conf.url, SIZE_MAX, &w->target) != 0) {
        fprintf(stderr, "unrecognized URL: %s\n", conf.url);
        exit(EXIT_FAILURE);
    }
    h2o_url_t *sp_target = &w->target;
    if (conf.connect_to != NULL) {
        if (h2o_url_parse(NULL, conf.connect_to, SIZE_MAX, &w->connect_to) != 0) {
            fprintf(stderr, "invalid -x URL: %s\n", conf.connect_to);
            exit(EXIT_FAILURE);
        }
        sp_target = &w->connect_to;
    }
    /* One connection pool per connection this worker drives. They share the worker's single QUIC
     * socket and event loop, so N pools become N QUIC connections multiplexed over one UDP socket -
     * which is how we model many client connections without one thread (and one socket) per connection. */
    if (w->n_conns < 1)
        w->n_conns = 1;
    w->connpools = h2o_mem_alloc(sizeof(*w->connpools) * w->n_conns);
    w->conn_up = h2o_mem_alloc(w->n_conns);
    memset(w->conn_up, 0, w->n_conns);
    /* One socketpool shared by all this worker's connection pools. For HTTP/3 the socketpool
     * holds no runtime resource (the UDP socket lives in the quic ctx); it is only read for the
     * target and address family. So one per worker suffices and avoids N event-loop timer
     * registrations that scaled the per-connection cost. Each connpool still keeps its own conn
     * list, so N connpools still fan out to N distinct QUIC connections. */
    h2o_socketpool_target_t *target = h2o_socketpool_create_target(sp_target, NULL);
    h2o_socketpool_init_specific(&w->sockpool, 4, &target, 1, NULL);
    h2o_socketpool_set_timeout(&w->sockpool, conf.io_timeout);
    h2o_socketpool_register_loop(&w->sockpool, w->loop);
    for (unsigned i = 0; i < w->n_conns; ++i)
        h2o_httpclient_connection_pool_init(&w->connpools[i], &w->sockpool);
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    worker_init(w);

    /* Start with a bounded active set and ramp up (see CONN_RAMP_* above): opening all n_conns
     * handshakes at once is the dominant failure source at high connection counts. target (streams
     * in flight) tracks the active set, so offered load ramps together with the connections. */
    w->active_conns = w->n_conns < CONN_RAMP_INITIAL ? w->n_conns : CONN_RAMP_INITIAL;

    for (;;) {
        unsigned target = w->active_conns * conf.concurrency;
        /* top up in-flight requests across the worker's active connections. With send_batch>1 we
         * wait until at least that many slots are free before refilling, so the started requests
         * queue together and quicly coalesces them into one datagram instead of one-per-flush. */
        if (target - w->inflight >= conf.send_batch || w->inflight == 0)
            while (w->inflight < target && may_start(w))
                start_one(w);
        if (w->inflight == 0) {
            if (!overall_has_more(w))
                break; /* request count reached, or -d deadline passed */
            if (conf.reconnect != 0 && w->conn_started >= conf.reconnect) {
                /* connection spent its episode budget; close it and let the next
                 * request re-establish - resuming via the cached ticket */
                close_and_drain(w);
                w->conn_started = 0;
                continue;
            }
            break; /* defensive: nothing startable and nothing in flight */
        }
        h2o_evloop_run(w->loop, INT32_MAX);
        h2o_quic_flush_datagrams(&w->h3ctx.h3); /* flush this pass's batched sends */
        /* grow the active set, but only while in-flight handshakes stay under the cap */
        if (w->active_conns < w->n_conns) {
            unsigned inflight_hs = w->active_conns - w->established;
            if (inflight_hs < CONN_MAX_HANDSHAKES) {
                unsigned room = CONN_MAX_HANDSHAKES - inflight_hs;
                w->active_conns += room < CONN_RAMP_STEP ? room : CONN_RAMP_STEP;
                if (w->active_conns > w->n_conns)
                    w->active_conns = w->n_conns;
            }
        }
    }

    /* flush QUIC CONNECTION_CLOSE so servers do not log the runs as aborts */
    close_and_drain(w);
    return NULL;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* p in [0,100] over an ascending-sorted array */
static double pctl_ms(const uint32_t *sorted, size_t n, double p)
{
    if (n == 0)
        return 0;
    size_t idx = (size_t)(p / 100.0 * (double)(n - 1) + 0.5);
    if (idx >= n)
        idx = n - 1;
    return sorted[idx] / 1000.0;
}

/* Number of CPUs actually available to this process, honoring Docker limits: the CPU set
 * (--cpuset-cpus, via sched_getaffinity) and a cgroup-v2 CPU quota (--cpus, via cpu.max). */
static unsigned detect_ncpu(void)
{
    unsigned n = 0;
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        n = (unsigned)CPU_COUNT(&set);
    if (n == 0) {
        long s = sysconf(_SC_NPROCESSORS_ONLN);
        n = s > 0 ? (unsigned)s : 1;
    }
    FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r"); /* "<quota> <period>" or "max <period>" */
    if (f != NULL) {
        char quota[32] = {0};
        unsigned long period = 0;
        if (fscanf(f, "%31s %lu", quota, &period) == 2 && period > 0 && strcmp(quota, "max") != 0) {
            unsigned long q = strtoul(quota, NULL, 10);
            unsigned cq = (unsigned)((q + period - 1) / period); /* ceil(quota/period) */
            if (cq >= 1 && cq < n)
                n = cq;
        }
        fclose(f);
    }
    return n >= 1 ? n : 1;
}

static void usage(void)
{
    fprintf(stderr,
            "Usage: %s [options] <url>\n"
            "  -n <count>   total requests to send (default: 100)\n"
            "  -d <seconds> run for this long instead of -n (overrides -n)\n"
            "  -c <num>     concurrent streams per connection (default: 10)\n"
            "  --connections <n> total connections across all threads (default: one per thread)\n"
            "  -t <num>     worker threads (default: all CPUs available to the process)\n"
            "  -m <method>  request method (default: GET)\n"
            "  -H <name:value>   add a request header (repeatable)\n"
            "  -x <url>     connect to this host:port instead of the URL's (pins a backend)\n"
            "  -W <bytes>   HTTP/3 receive window (per stream)\n"
            "  -k           skip TLS certificate verification\n"
            "  --reconnect <N>   close each connection after N requests (exercises 0-RTT)\n"
            "  --send-batch <N>  accumulate N freed slots before refilling, so requests pack into\n"
            "                    fewer datagrams (1 = off; helps server-bound, hurts client-bound)\n"
            "  --no-resumption   force a full handshake on every connection\n"
            "  --max-udp-payload-size <bytes>\n"
            "  --initial-udp-payload-size <bytes>\n"
            "  --ack-frequency <0..1>\n"
            "  --disallow-delayed-ack\n"
            "  --no-ecn\n"
            "  --qpack-table <bytes>   QPACK encoder table capacity (default: 4096)\n"
            "  --key-exchange <name>   override the TLS key exchange (e.g. x25519)\n"
            "  -h           this help\n",
            progname);
}

static void add_header(const char *arg)
{
    const char *colon = strchr(arg, ':');
    if (colon == NULL) {
        fprintf(stderr, "no ':' in -H value\n");
        exit(EXIT_FAILURE);
    }
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
        ++value;
    if (conf.num_headers >= sizeof(conf.headers) / sizeof(conf.headers[0])) {
        fprintf(stderr, "too many headers\n");
        exit(EXIT_FAILURE);
    }
    h2o_iovec_t name = h2o_strdup(NULL, arg, colon - arg);
    h2o_strtolower(name.base, name.len);
    conf.headers[conf.num_headers].name = name;
    conf.headers[conf.num_headers].value = h2o_iovec_init(value, strlen(value));
    ++conf.num_headers;
}

static unsigned print_summary(struct worker *workers, double elapsed);

int main(int argc, char **argv)
{
    progname = argv[0];
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    enum {
        OPT_MAX_UDP = 0x100,
        OPT_INIT_UDP,
        OPT_ACK_FREQ,
        OPT_NO_DELAYED_ACK,
        OPT_NO_ECN,
        OPT_QPACK_TABLE,
        OPT_KEY_EXCHANGE,
        OPT_RECONNECT,
        OPT_NO_RESUMPTION,
        OPT_SEND_BATCH,
        OPT_CONNECTIONS,
    };
    static struct option longopts[] = {{"max-udp-payload-size", required_argument, NULL, OPT_MAX_UDP},
                                        {"initial-udp-payload-size", required_argument, NULL, OPT_INIT_UDP},
                                        {"ack-frequency", required_argument, NULL, OPT_ACK_FREQ},
                                        {"disallow-delayed-ack", no_argument, NULL, OPT_NO_DELAYED_ACK},
                                        {"no-ecn", no_argument, NULL, OPT_NO_ECN},
                                        {"qpack-table", required_argument, NULL, OPT_QPACK_TABLE},
                                        {"key-exchange", required_argument, NULL, OPT_KEY_EXCHANGE},
                                        {"reconnect", required_argument, NULL, OPT_RECONNECT},
                                        {"no-resumption", no_argument, NULL, OPT_NO_RESUMPTION},
                                        {"send-batch", required_argument, NULL, OPT_SEND_BATCH},
                                        {"connections", required_argument, NULL, OPT_CONNECTIONS},
                                        {"help", no_argument, NULL, 'h'},
                                        {NULL}};
    int opt;
    while ((opt = getopt_long(argc, argv, "n:c:t:d:m:H:x:W:kh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'n':
            conf.total_requests = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            conf.duration = atof(optarg);
            break;
        case 'c':
            conf.concurrency = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 't':
            conf.threads = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'm':
            conf.method = optarg;
            break;
        case 'H':
            add_header(optarg);
            break;
        case 'x':
            conf.connect_to = optarg;
            break;
        case 'W':
            conf.recv_window = strtoull(optarg, NULL, 10);
            break;
        case 'k':
            conf.verify_none = 1;
            break;
        case OPT_MAX_UDP:
            conf.max_udp_payload_size = strtoull(optarg, NULL, 10);
            break;
        case OPT_INIT_UDP:
            conf.initial_udp_payload_size = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_ACK_FREQ:
            conf.ack_frequency = atof(optarg);
            conf.have_ack_frequency = 1;
            break;
        case OPT_NO_DELAYED_ACK:
            conf.disallow_delayed_ack = 1;
            break;
        case OPT_NO_ECN:
            conf.no_ecn = 1;
            break;
        case OPT_QPACK_TABLE:
            conf.qpack_table = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_KEY_EXCHANGE:
            conf.key_exchange = optarg;
            break;
        case OPT_RECONNECT:
            conf.reconnect = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_NO_RESUMPTION:
            conf.no_resumption = 1;
            break;
        case OPT_SEND_BATCH:
            conf.send_batch = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_CONNECTIONS:
            conf.connections = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'h':
            usage();
            return 0;
        default:
            usage();
            return EXIT_FAILURE;
        }
    }
    argc -= optind;
    argv += optind;
    if (argc < 1) {
        usage();
        return EXIT_FAILURE;
    }
    conf.url = argv[0];

    if (conf.concurrency < 1)
        conf.concurrency = 1;
    { /* --send-batch <N> (or H3X_SEND_BATCH) holds freed slots until N accumulate, then starts them
         together so quicly packs the requests into fewer datagrams (send-side coalescing); 1 = off */
        const char *e;
        if (conf.send_batch == 0 && (e = getenv("H3X_SEND_BATCH")) != NULL)
            conf.send_batch = (unsigned)strtoul(e, NULL, 10);
        if (conf.send_batch < 1)
            conf.send_batch = 1;
        if (conf.send_batch > conf.concurrency)
            conf.send_batch = conf.concurrency;
    }
    if (conf.threads < 1)
        conf.threads = detect_ncpu(); /* default: use every CPU the container gives us */
    if (conf.total_requests < 1)
        conf.total_requests = 1;
    if (conf.duration == 0 && conf.threads > conf.total_requests)
        conf.threads = conf.total_requests;
    /* connections: total across all threads, default one per thread. Fewer connections than threads
     * would leave threads idle, so trim the thread count to match. */
    if (conf.connections == 0)
        conf.connections = conf.threads;
    if (conf.connections < conf.threads)
        conf.threads = conf.connections;
    /* reconnect (0-RTT churn) tracks a single connection per worker, so it needs 1 conn per thread */
    if (conf.reconnect != 0 && conf.connections > conf.threads) {
        fprintf(stderr, "--reconnect needs one connection per thread; use --connections %u or fewer\n", conf.threads);
        return EXIT_FAILURE;
    }
    if (conf.key_exchange != NULL) {
        ptls_key_exchange_algorithm_t **named;
        for (named = ptls_openssl_key_exchanges_all; *named != NULL; ++named)
            if (strcasecmp((*named)->name, conf.key_exchange) == 0)
                break;
        if (*named == NULL) {
            fprintf(stderr, "unknown key exchange: %s\n", conf.key_exchange);
            return EXIT_FAILURE;
        }
        h3_key_exchanges[0] = *named;
        h3_key_exchanges[1] = NULL;
    } else {
        size_t i = 0;
#if PTLS_OPENSSL_HAVE_X25519
        h3_key_exchanges[i++] = &ptls_openssl_x25519;
#endif
        h3_key_exchanges[i++] = &ptls_openssl_secp256r1;
    }

    struct worker *workers = h2o_mem_alloc(sizeof(*workers) * conf.threads);
    memset(workers, 0, sizeof(*workers) * conf.threads);
    unsigned base = conf.total_requests / conf.threads, rem = conf.total_requests % conf.threads;
    unsigned conns_base = conf.connections / conf.threads, conns_rem = conf.connections % conf.threads;

    char budget[32];
    if (conf.duration != 0)
        snprintf(budget, sizeof budget, "%gs", conf.duration);
    else
        snprintf(budget, sizeof budget, "%u requests", conf.total_requests);
    fprintf(stderr, "h3x -> %s  (%u threads, %u conns x %u streams, %s%s)\n", conf.url, conf.threads,
            conf.connections, conf.concurrency, budget, conf.reconnect != 0 ? ", reconnect mode" : "");

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    if (conf.duration != 0) {
        uint64_t deadline_us = (uint64_t)t0.tv_sec * 1000000 + t0.tv_usec + (uint64_t)(conf.duration * 1e6);
        g_deadline.tv_sec = deadline_us / 1000000;
        g_deadline.tv_usec = deadline_us % 1000000;
    }
    for (unsigned i = 0; i < conf.threads; ++i) {
        workers[i].idx = i;
        workers[i].req_target = base + (i < rem ? 1 : 0);
        workers[i].n_conns = conns_base + (i < conns_rem ? 1 : 0);
        if (pthread_create(&workers[i].tid, NULL, worker_main, &workers[i]) != 0) {
            perror("pthread_create");
            return EXIT_FAILURE;
        }
    }
    for (unsigned i = 0; i < conf.threads; ++i)
        pthread_join(workers[i].tid, NULL);
    gettimeofday(&t1, NULL);

    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    unsigned n_fail = print_summary(workers, elapsed);
    free(workers);
    return n_fail != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Merge the per-worker counters and latency samples, print the run summary, and return total failed. */
static unsigned print_summary(struct worker *workers, double elapsed)
{
    unsigned n_ok = 0, n_fail = 0, n_resumed = 0, conns = 0;
    uint64_t bytes = 0;
    size_t total_lat = 0;
    for (unsigned i = 0; i < conf.threads; ++i) {
        n_ok += workers[i].n_ok;
        n_fail += workers[i].n_fail;
        n_resumed += workers[i].n_resumed;
        conns += workers[i].conn_count;
        bytes += workers[i].body_bytes;
        total_lat += workers[i].lat_n;
    }
    uint32_t *lat = h2o_mem_alloc(sizeof(uint32_t) * (total_lat != 0 ? total_lat : 1));
    size_t off = 0;
    for (unsigned i = 0; i < conf.threads; ++i) {
        memcpy(lat + off, workers[i].lat_us, workers[i].lat_n * sizeof(uint32_t));
        off += workers[i].lat_n;
        free(workers[i].lat_us);
    }
    qsort(lat, total_lat, sizeof(uint32_t), cmp_u32);

    double mean = 0;
    for (size_t i = 0; i < total_lat; ++i)
        mean += lat[i];
    if (total_lat != 0)
        mean = mean / total_lat / 1000.0;

    fprintf(stderr, "\n");
    fprintf(stderr, "completed:   %u    failed: %u\n", n_ok, n_fail);
    /* in reconnect mode conn_count tallies the churned episodes; otherwise every pool opens exactly
     * one connection, so the intended total is the accurate figure */
    fprintf(stderr, "connections: %u    resumed (PSK/0-RTT): %u requests\n",
            conf.reconnect != 0 ? conns : conf.connections, n_resumed);
    fprintf(stderr, "duration:    %.3f s\n", elapsed);
    fprintf(stderr, "throughput:  %.0f req/s    %.2f MB/s\n", elapsed > 0 ? n_ok / elapsed : 0,
            elapsed > 0 ? bytes / elapsed / (1024 * 1024) : 0);
    fprintf(stderr, "latency ms:  min %.2f  mean %.2f  p50 %.2f  p90 %.2f  p99 %.2f  max %.2f\n", pctl_ms(lat, total_lat, 0),
            mean, pctl_ms(lat, total_lat, 50), pctl_ms(lat, total_lat, 90), pctl_ms(lat, total_lat, 99),
            pctl_ms(lat, total_lat, 100));

    free(lat);
    return n_fail;
}
