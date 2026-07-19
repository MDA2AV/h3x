/* Per-thread setup (QUIC/TLS context, connection pools) and the worker event loop. */
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>
#include "picotls/openssl.h"
#include "h2o/hostinfo.h"
#include "h3x.h"

/* Connection-establishment pacing. Opening every connection's handshake at once floods a worker's
 * event loop at high connection counts, and a chunk of the handshakes time out - the dominant
 * failure source in the many-connection regime. Instead open an initial set and grow it only as
 * connections come up, keeping the number of in-flight (not-yet-established) handshakes under a cap. */
#define CONN_RAMP_INITIAL 32   /* handshakes a worker starts immediately */
#define CONN_RAMP_STEP 8       /* connections added per loop pass while ramping */
#define CONN_MAX_HANDSHAKES 32 /* cap on concurrent in-flight handshakes per worker */

static const char *ca_bundle(void)
{
    const char *e = getenv("H3X_CA_BUNDLE");
    return e != NULL ? e : "/etc/ssl/certs/ca-certificates.crt";
}

static void setup_connections(struct worker *w);

/* Build one HTTP/3 (quicly + picotls) context: TLS, QUIC transport params and tuning knobs,
 * resumption/0-RTT, certificate verification, and its UDP socket(s). In shared-socket mode this is
 * built once per worker and all its connections multiplex over it (they share the worker's
 * 4-tuple, distinguished by connection ID); in --socket-per-conn mode it is built once per
 * connection so every connection gets its own socket and 4-tuple. The resumption cache stays
 * per-worker either way. */
static void init_h3_ctx(struct worker *w, h2o_http3client_ctx_t *h3ctx, quicly_cid_plaintext_t *next_cid)
{
    *h3ctx = (h2o_http3client_ctx_t){
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
    quicly_amend_ptls_context(&h3ctx->tls);
    h3ctx->quic = quicly_spec_context;
    h3ctx->quic.transport_params.max_streams_uni = 10;
    h3ctx->quic.tls = &h3ctx->tls;
    {
        uint8_t random_key[PTLS_SHA256_DIGEST_SIZE];
        h3ctx->tls.random_bytes(random_key, sizeof(random_key));
        h3ctx->quic.cid_encryptor = quicly_new_default_cid_encryptor(
            &ptls_openssl_quiclb, &ptls_openssl_aes128ecb, &ptls_openssl_sha256, ptls_iovec_init(random_key, sizeof(random_key)));
        ptls_clear_memory(random_key, sizeof(random_key));
    }
    h3ctx->quic.stream_open = &h2o_httpclient_http3_on_stream_open;

    /* transport tuning knobs */
    if (conf.recv_window != 0) {
        h3ctx->quic.transport_params.max_stream_data.uni = conf.recv_window;
        h3ctx->quic.transport_params.max_stream_data.bidi_local = conf.recv_window;
        h3ctx->quic.transport_params.max_stream_data.bidi_remote = conf.recv_window;
    }
    if (conf.max_udp_payload_size != 0)
        h3ctx->quic.transport_params.max_udp_payload_size = conf.max_udp_payload_size;
    if (conf.initial_udp_payload_size != 0)
        h3ctx->quic.initial_egress_max_udp_payload_size = conf.initial_udp_payload_size;
    if (conf.disallow_delayed_ack)
        h3ctx->quic.transport_params.min_ack_delay_usec = UINT64_MAX;
    if (conf.have_ack_frequency)
        h3ctx->quic.ack_frequency = (uint16_t)(conf.ack_frequency * 1024);
    if (conf.no_ecn)
        h3ctx->quic.enable_ratio.ecn = 0;

    /* resumption / 0-RTT (cache is per worker, shared by all its contexts) */
    if (!conf.no_resumption) {
        w->save_ticket.cb = save_ticket_cb;
        h3ctx->tls.save_ticket = &w->save_ticket;
        w->save_token.cb = save_token_cb;
        h3ctx->quic.save_resumption_token = &w->save_token;
        h3ctx->load_session = load_session_cb;
    }

    if (!conf.verify_none) {
        X509_STORE *store = X509_STORE_new();
        if (store == NULL || X509_STORE_load_locations(store, ca_bundle(), NULL) != 1) {
            fprintf(stderr, "failed to load CA bundle: %s (set H3X_CA_BUNDLE or pass -k)\n", ca_bundle());
            exit(EXIT_FAILURE);
        }
        ptls_openssl_init_verify_certificate(&h3ctx->verify_cert, store);
        X509_STORE_free(store);
        h3ctx->tls.verify_certificate = &h3ctx->verify_cert.super;
    }

    /* per-thread CID space, so connection IDs across threads never collide */
    *next_cid = (quicly_cid_plaintext_t){.thread_id = w->idx};
    h2o_socket_t *socks[2], **sp = socks;
    if ((*sp = h2o_quic_create_client_socket(w->loop, AF_INET)) != NULL)
        ++sp;
    if ((*sp = h2o_quic_create_client_socket(w->loop, AF_INET6)) != NULL)
        ++sp;
    if (sp == socks) {
        perror("failed to create UDP socket");
        exit(EXIT_FAILURE);
    }
    h2o_quic_init_context(&h3ctx->h3, w->loop, socks[0], sp > socks + 1 ? socks[1] : NULL, &h3ctx->quic, next_cid, NULL,
                          h2o_httpclient_http3_notify_connection_update, 1 /* use_gso */, NULL);
    /* coalesce all this context's sends into one sendmmsg per loop pass; pointless with a single
     * connection per socket, so off in --socket-per-conn mode */
    h3ctx->h3.batch_sends = !conf.socket_per_conn;
}

/* Fill the generic client context: timeouts, buffer size, protocol ratio, and the DNS receiver. */
static void init_client_ctx(struct worker *w, h2o_httpclient_ctx_t *ctx, h2o_http3client_ctx_t *h3ctx)
{
    *ctx = (h2o_httpclient_ctx_t){
        .loop = w->loop,
        .getaddr_receiver = &w->getaddr_receiver,
        .io_timeout = conf.io_timeout,
        .connect_timeout = conf.io_timeout,
        .first_byte_timeout = conf.io_timeout,
        .keepalive_timeout = conf.io_timeout,
        .max_buffer_size = 128 * 1024,
        .http3 = h3ctx,
    };
    ctx->protocol_selector.ratio.http3 = 100; /* H3 only */
}

static void worker_init(struct worker *w)
{
    w->loop = h2o_evloop_create();
    w->queue = h2o_multithread_create_queue(w->loop);
    h2o_multithread_register_receiver(w->queue, &w->getaddr_receiver, h2o_hostinfo_getaddr_receiver);

    if (!conf.socket_per_conn) {
        init_h3_ctx(w, &w->h3ctx, &w->next_cid);
        init_client_ctx(w, &w->ctx, &w->h3ctx);
    }

    setup_connections(w);
}

/* Parse the target URL and set up this worker's connection pools. In shared-socket mode one
 * socketpool is shared by all the worker's connpools and each connpool becomes one QUIC connection
 * over the worker's single UDP socket. In --socket-per-conn mode each connection additionally gets
 * its own unit (socket + quic ctx + client ctx), so its 4-tuple is unique. */
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
    /* One connection pool per connection this worker drives. In shared-socket mode they share the
     * worker's single QUIC socket and event loop, so N pools become N QUIC connections multiplexed
     * over one UDP socket - which is how we model many client connections without one thread (and
     * one socket) per connection. */
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
    if (conf.socket_per_conn) {
        w->units = h2o_mem_alloc(sizeof(*w->units) * w->n_conns);
        memset(w->units, 0, sizeof(*w->units) * w->n_conns);
        for (unsigned i = 0; i < w->n_conns; ++i) {
            init_h3_ctx(w, &w->units[i].h3ctx, &w->units[i].next_cid);
            init_client_ctx(w, &w->units[i].ctx, &w->units[i].h3ctx);
            h2o_httpclient_connection_pool_init(&w->connpools[i], &w->sockpool);
        }
    } else {
        for (unsigned i = 0; i < w->n_conns; ++i)
            h2o_httpclient_connection_pool_init(&w->connpools[i], &w->sockpool);
    }
}

void *worker_main(void *arg)
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
        if (!conf.socket_per_conn)
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
