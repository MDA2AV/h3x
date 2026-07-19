/* Session resumption callbacks (wired unless --no-resumption): the in-memory
 * ticket/token cache that lets churned connections resume with PSK / 0-RTT. */
#include <stdlib.h>
#include <string.h>
#include "h3x.h"

int load_session_cb(h2o_httpclient_ctx_t *ctx, struct sockaddr *server_addr, const char *server_name,
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

int save_ticket_cb(ptls_save_ticket_t *self, ptls_t *tls, ptls_iovec_t src)
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

quicly_error_t save_token_cb(quicly_save_resumption_token_t *self, quicly_conn_t *conn, ptls_iovec_t token)
{
    struct worker *w = H2O_STRUCT_FROM_MEMBER(struct worker, save_token, self);
    free(w->sess.token.base);
    w->sess.token.base = h2o_mem_alloc(token.len);
    memcpy(w->sess.token.base, token.base, token.len);
    w->sess.token.len = token.len;
    return 0;
}
