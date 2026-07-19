/* Request lifecycle: the closed-loop driver's start/finish path and the
 * httpclient callbacks (connect -> head -> body). */
#include <stdlib.h>
#include <string.h>
#include "h3x.h"

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

/* ---- run budget: a fixed request count, or a wall-clock duration via -d ---- */

struct timeval g_deadline; /* absolute stop time; set in main when -d is used */

static int deadline_reached(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec > g_deadline.tv_sec ||
           (now.tv_sec == g_deadline.tv_sec && now.tv_usec >= g_deadline.tv_usec);
}

int overall_has_more(struct worker *w)
{
    return conf.duration != 0 ? !deadline_reached() : w->started < w->req_target;
}

int may_start(struct worker *w)
{
    if (!overall_has_more(w))
        return 0;
    if (conf.reconnect != 0 && w->conn_started >= conf.reconnect)
        return 0;
    return 1;
}

void close_and_drain(struct worker *w)
{
    if (w->units != NULL) { /* --socket-per-conn: one quic ctx per connection */
        for (unsigned i = 0; i < w->n_conns; ++i)
            h2o_quic_close_all_connections(&w->units[i].h3ctx.h3);
        for (;;) {
            size_t remaining = 0;
            for (unsigned i = 0; i < w->n_conns; ++i)
                remaining += h2o_quic_num_connections(&w->units[i].h3ctx.h3);
            if (remaining == 0)
                break;
            h2o_evloop_run(w->loop, 100);
        }
        return;
    }
    h2o_quic_close_all_connections(&w->h3ctx.h3);
    while (h2o_quic_num_connections(&w->h3ctx.h3) != 0) {
        h2o_evloop_run(w->loop, 100);
        h2o_quic_flush_datagrams(&w->h3ctx.h3);
    }
}

/* ---- request lifecycle ---- */

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

void start_one(struct worker *w)
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
    /* in --socket-per-conn mode each connection has its own client ctx (own quic ctx + socket) */
    h2o_httpclient_ctx_t *cctx = w->units != NULL ? &w->units[rc->conn_idx].ctx : &w->ctx;
    h2o_httpclient_connect(NULL, pool, rc, cctx, cp, &w->target, NULL, on_connect);
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
