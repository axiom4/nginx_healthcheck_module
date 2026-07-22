
/*
 * ngx_http_hc_probe.c — the active check state machine: per-worker timers
 * (worker 0 only), connect, optional TLS handshake, request send, response
 * receive/framing, match evaluation, and shared-memory state update.
 */

#include "ngx_http_upstream_healthcheck_module.h"


static void ngx_http_hc_begin_check(ngx_event_t *ev);
static ngx_int_t ngx_http_hc_dummy_get_peer(ngx_peer_connection_t *pc,
    void *data);
#if (NGX_SSL)
static void ngx_http_hc_ssl_handshake_start(ngx_event_t *ev);
static void ngx_http_hc_ssl_handshake_done(ngx_connection_t *c);
#endif
static void ngx_http_hc_send_handler(ngx_event_t *ev);
static u_char *ngx_http_hc_find_header(u_char *start, u_char *end,
    const char *name);
static ngx_flag_t ngx_http_hc_header_value_has(u_char *start, u_char *end,
    const char *needle);
static ngx_int_t ngx_http_hc_response_ready(ngx_http_hc_peer_t *peer);
static void ngx_http_hc_recv_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_hc_parse_response(ngx_http_hc_peer_t *peer);
static void ngx_http_hc_idle_closed_handler(ngx_event_t *ev);
static void ngx_http_hc_finish_check(ngx_http_hc_peer_t *peer,
    ngx_int_t success);


ngx_int_t
ngx_http_hc_init_process(ngx_cycle_t *cycle)
{
    ngx_uint_t                i;
    ngx_http_hc_peer_t       *peers;
    ngx_http_hc_main_conf_t  *hmcf;

    hmcf = ngx_http_cycle_get_module_main_conf(cycle,
               ngx_http_upstream_healthcheck_module);

    if (hmcf == NULL || hmcf->peers.nelts == 0 || hmcf->shm_peers == NULL) {
        return NGX_OK;
    }

    /* probe from a single worker only */
    if (ngx_worker != 0) {
        return NGX_OK;
    }

    peers = hmcf->peers.elts;

    for (i = 0; i < hmcf->peers.nelts; i++) {
        peers[i].check_ev.handler = ngx_http_hc_begin_check;
        peers[i].check_ev.data    = &peers[i];
        peers[i].check_ev.log     = cycle->log;

        /* spread the first probes over the interval */
        ngx_add_timer(&peers[i].check_ev,
                      ngx_random() % peers[i].conf->interval + 1);

        if (peers[i].hostname.len) {
            peers[i].resolve_ev.handler = ngx_http_hc_resolve_begin;
            peers[i].resolve_ev.data    = &peers[i];
            peers[i].resolve_ev.log     = cycle->log;

            ngx_add_timer(&peers[i].resolve_ev,
                          ngx_random() % peers[i].conf->resolve_interval + 1);
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_hc_dummy_get_peer(ngx_peer_connection_t *pc, void *data)
{
    return NGX_OK;
}


static void
ngx_http_hc_begin_check(ngx_event_t *ev)
{
    ngx_http_hc_peer_t  *peer = ev->data;
    ngx_int_t            rc;
    ngx_connection_t    *c;

    ngx_add_timer(ev, peer->conf->interval);

    if (peer->busy) {
        return;
    }

    peer->recv_len       = 0;
    peer->header_end      = -1;
    peer->content_length  = -1;

    if (peer->request) {
        /* reuse the buffer built on the first probe */
        peer->request->pos = peer->request->start;
    }

    if (peer->conn_open) {

        /* reuse the connection kept idle from the previous cycle instead
         * of paying for a new connect (and, for ssl, a new handshake) */

        c = peer->pc.connection;

        if (c->read->timer_set) {
            ngx_del_timer(c->read);
        }

        peer->busy      = 1;
        peer->conn_open = 0;

        c->write->handler = ngx_http_hc_send_handler;
        c->read->handler  = ngx_http_hc_recv_handler;

        ngx_add_timer(c->write, peer->conf->timeout);

        ngx_http_hc_send_handler(c->write);
        return;
    }

    ngx_memzero(&peer->pc, sizeof(ngx_peer_connection_t));

    peer->pc.sockaddr  = peer->sockaddr;
    peer->pc.socklen   = peer->socklen;
    peer->pc.name      = &peer->name;
    peer->pc.get       = ngx_http_hc_dummy_get_peer;
    peer->pc.log       = ev->log;
    peer->pc.log_error = NGX_ERROR_ERR;

    rc = ngx_event_connect_peer(&peer->pc);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_http_hc_finish_check(peer, 0);
        return;
    }

    peer->busy = 1;

    c = peer->pc.connection;
    c->data = peer;

#if (NGX_SSL)
    if (peer->conf->ssl) {
        c->pool = ngx_create_pool(1024, ev->log);
        if (c->pool == NULL) {
            ngx_http_hc_finish_check(peer, 0);
            return;
        }

        c->write->handler = ngx_http_hc_ssl_handshake_start;
        c->read->handler  = ngx_http_hc_ssl_handshake_start;

    } else
#endif
    {
        c->pool           = NULL;
        c->write->handler = ngx_http_hc_send_handler;
        c->read->handler  = ngx_http_hc_recv_handler;
    }

    ngx_add_timer(c->write, peer->conf->timeout);

    if (rc == NGX_OK) {
#if (NGX_SSL)
        if (peer->conf->ssl) {
            ngx_http_hc_ssl_handshake_start(c->write);
        } else
#endif
        {
            ngx_http_hc_send_handler(c->write);
        }
    }
}


#if (NGX_SSL)

static void
ngx_http_hc_ssl_handshake_start(ngx_event_t *ev)
{
    ngx_connection_t       *c    = ev->data;
    ngx_http_hc_peer_t     *peer = c->data;
    ngx_http_hc_srv_conf_t *hscf = peer->conf;
    ngx_int_t                rc;

    if (ev->timedout) {
        ngx_http_hc_finish_check(peer, 0);
        return;
    }

    if (c->ssl == NULL) {

        if (ngx_ssl_create_connection(hscf->ssl_ctx, c,
                                      NGX_SSL_BUFFER|NGX_SSL_CLIENT)
            != NGX_OK)
        {
            ngx_http_hc_finish_check(peer, 0);
            return;
        }

        c->sendfile = 0;

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
        if (hscf->ssl_name.len) {
            /* SSL_set_tlsext_host_name() needs a null-terminated string */

            u_char *host = ngx_pnalloc(c->pool, hscf->ssl_name.len + 1);
            if (host == NULL) {
                ngx_http_hc_finish_check(peer, 0);
                return;
            }

            (void) ngx_cpystrn(host, hscf->ssl_name.data,
                               hscf->ssl_name.len + 1);

            if (SSL_set_tlsext_host_name(c->ssl->connection, host) == 0) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                    "healthcheck: SSL_set_tlsext_host_name(\"%s\") failed",
                    host);
                ngx_http_hc_finish_check(peer, 0);
                return;
            }
        }
#endif

        if (peer->ssl_session) {
            /* best-effort session resumption: skips the full handshake
             * (cert exchange, key negotiation) on the backend when it
             * still recognizes the session from a previous probe cycle */
            SSL_set_session(c->ssl->connection, peer->ssl_session);
        }

        c->ssl->handler = ngx_http_hc_ssl_handshake_done;
    }

    rc = ngx_ssl_handshake(c);

    if (rc == NGX_ERROR) {
        ngx_http_hc_finish_check(peer, 0);
        return;
    }

    if (rc == NGX_AGAIN) {
        /* ngx_ssl_handshake() already re-armed read/write handlers;
         * it will call c->ssl->handler() once the handshake settles */
        return;
    }

    ngx_http_hc_ssl_handshake_done(c);
}


static void
ngx_http_hc_ssl_handshake_done(ngx_connection_t *c)
{
    ngx_http_hc_peer_t  *peer = c->data;

    if (!c->ssl->handshaked) {
        ngx_http_hc_finish_check(peer, 0);
        return;
    }

    if (peer->conf->ssl_verify) {
        if (SSL_get_verify_result(c->ssl->connection) != X509_V_OK) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                "healthcheck: invalid TLS certificate for %V (%V)",
                &peer->name, &peer->upstream);
            ngx_http_hc_finish_check(peer, 0);
            return;
        }

        /* chain verification alone doesn't confirm the cert belongs to
         * this host; check it against ssl_name when one was configured
         * (with no ssl_name there is no hostname to check against, since
         * peer->name is a bare ip:port) */

        if (peer->conf->ssl_name.len
            && ngx_ssl_check_host(c, &peer->conf->ssl_name) != NGX_OK)
        {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                "healthcheck: the TLS certificate of %V (%V) doesn't "
                "cover hostname \"%V\"",
                &peer->name, &peer->upstream, &peer->conf->ssl_name);
            ngx_http_hc_finish_check(peer, 0);
            return;
        }
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }
    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    c->write->handler = ngx_http_hc_send_handler;
    c->read->handler  = ngx_http_hc_recv_handler;

    ngx_add_timer(c->write, peer->conf->timeout);

    ngx_http_hc_send_handler(c->write);
}

#endif /* NGX_SSL */


static void
ngx_http_hc_send_handler(ngx_event_t *ev)
{
    ngx_connection_t    *c    = ev->data;
    ngx_http_hc_peer_t  *peer = c->data;
    ssize_t              n, size;
    u_char               buf[512];
    u_char              *p;

    if (ev->timedout) {
        ngx_http_hc_finish_check(peer, 0);
        return;
    }

    if (peer->request == NULL) {

        if (peer->conf->keepalive) {
            p = ngx_snprintf(buf, sizeof(buf),
                    "GET %V HTTP/1.1" CRLF
                    "Host: %V" CRLF
                    "User-Agent: nginx-healthcheck" CRLF
                    "Connection: keep-alive" CRLF CRLF,
                    &peer->conf->uri, &peer->upstream);
        } else {
            p = ngx_snprintf(buf, sizeof(buf),
                    "GET %V HTTP/1.0" CRLF
                    "Host: %V" CRLF
                    "User-Agent: nginx-healthcheck" CRLF
                    "Connection: close" CRLF CRLF,
                    &peer->conf->uri, &peer->upstream);
        }

        peer->request = ngx_create_temp_buf(ngx_cycle->pool, p - buf);
        if (peer->request == NULL) {
            ngx_http_hc_finish_check(peer, 0);
            return;
        }

        peer->request->last = ngx_cpymem(peer->request->pos, buf, p - buf);
    }

    while (peer->request->pos < peer->request->last) {

        size = peer->request->last - peer->request->pos;
        n = c->send(c, peer->request->pos, size);

        if (n == NGX_AGAIN) {
            return;
        }

        if (n == NGX_ERROR) {
            ngx_http_hc_finish_check(peer, 0);
            return;
        }

        peer->request->pos += n;
    }

    /* request fully sent: wait for the response */

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    ngx_add_timer(c->read, peer->conf->timeout);

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_http_hc_finish_check(peer, 0);
    }
}


/*
 * Locates a "Name:" header (case-insensitive) within recv[0, header_end)
 * and returns a pointer to the first byte of its value, or NULL.
 */
static u_char *
ngx_http_hc_find_header(u_char *start, u_char *end, const char *name)
{
    size_t   len = ngx_strlen(name);
    u_char  *p, *value;

    if ((size_t) (end - start) < len) {
        return NULL;
    }

    for (p = start; p <= end - len; p++) {
        if ((p == start || p[-1] == '\n')
            && ngx_strncasecmp(p, (u_char *) name, len) == 0)
        {
            value = p + len;
            while (value < end && *value == ' ') {
                value++;
            }
            return value;
        }
    }

    return NULL;
}


/*
 * Case-insensitive search for "needle" within a single header's value,
 * bounded to that line (start up to the next CR/LF or "end", whichever
 * comes first) so a match can't spill over into the response body.
 */
static ngx_flag_t
ngx_http_hc_header_value_has(u_char *start, u_char *end, const char *needle)
{
    size_t   len = ngx_strlen(needle);
    u_char  *line_end, *p;

    line_end = start;
    while (line_end < end && *line_end != '\r' && *line_end != '\n') {
        line_end++;
    }

    if ((size_t) (line_end - start) < len) {
        return 0;
    }

    for (p = start; p <= line_end - len; p++) {
        if (ngx_strncasecmp(p, (u_char *) needle, len) == 0) {
            return 1;
        }
    }

    return 0;
}


/*
 * After each recv(), checks whether a full, unambiguously-framed response
 * has been received: headers plus, when "Content-Length" is present, the
 * exact body length it announces. Sets peer->header_end/content_length as
 * a side effect (cached across calls within the same probe cycle) so the
 * response doesn't need to be re-scanned from byte zero every time.
 *
 * Returns 1 when the response is fully framed and ready to evaluate, 0
 * otherwise (more data needed, or the framing is ambiguous and reading
 * must continue until the connection closes, as with plain "close"-only
 * probes).
 */
static ngx_int_t
ngx_http_hc_response_ready(ngx_http_hc_peer_t *peer)
{
    u_char     *end, *cl, *conn;
    ngx_int_t   n;

    if (peer->header_end < 0) {
        end = (u_char *) ngx_strstr(peer->recv, "\r\n\r\n");
        if (end == NULL) {
            return 0;
        }

        peer->header_end = (end - peer->recv) + 4;

        cl = ngx_http_hc_find_header(peer->recv, end, "Content-Length:");
        if (cl != NULL) {
            u_char *line_end = cl;

            while (line_end < end && *line_end != '\r' && *line_end != '\n')
            {
                line_end++;
            }

            n = ngx_atoi(cl, line_end - cl);
            if (n >= 0) {
                peer->content_length = n;
            }
        }

        peer->keepalive_ok = 0;

        if (peer->conf->keepalive && peer->content_length >= 0) {
            conn = ngx_http_hc_find_header(peer->recv, end, "Connection:");

            if (conn == NULL
                || !ngx_http_hc_header_value_has(conn, end, "close"))
            {
                peer->keepalive_ok = 1;
            }
        }
    }

    if (peer->content_length < 0) {
        /* framing is ambiguous: keep reading until close/buffer-full */
        return 0;
    }

    return (ngx_int_t) peer->recv_len
               >= peer->header_end + peer->content_length;
}


static void
ngx_http_hc_recv_handler(ngx_event_t *ev)
{
    ngx_connection_t    *c    = ev->data;
    ngx_http_hc_peer_t  *peer = c->data;
    ssize_t              n;

    if (ev->timedout) {
        ngx_http_hc_finish_check(peer, 0);
        return;
    }

    for ( ;; ) {

        n = c->recv(c, peer->recv + peer->recv_len,
                    NGX_HC_RECV_SIZE - peer->recv_len - 1);

        if (n == NGX_AGAIN) {
            return;
        }

        if (n == NGX_ERROR || n == 0) {
            peer->keepalive_ok = 0;
            break;
        }

        peer->recv_len += n;
        peer->recv[peer->recv_len] = '\0';

        if (ngx_http_hc_response_ready(peer)) {
            break;
        }

        if (peer->recv_len >= NGX_HC_RECV_SIZE - 1) {
            peer->keepalive_ok = 0;
            break;
        }
    }

    /*
     * either the response is fully framed (possibly reusable for the next
     * probe cycle), or the connection was closed / the buffer filled up:
     * evaluate what we have against the configured (or default) match
     * rules either way
     */

    peer->recv[peer->recv_len] = '\0';

    ngx_http_hc_finish_check(peer, ngx_http_hc_parse_response(peer));
}


static ngx_int_t
ngx_http_hc_parse_response(ngx_http_hc_peer_t *peer)
{
    ngx_http_hc_srv_conf_t  *hscf = peer->conf;
    u_char                  *p, *body;
    ngx_uint_t                status, status_min, status_max;
    ngx_str_t                 body_str;
    int                       captures[3];

    p = peer->recv;

    if (peer->recv_len < 12 || ngx_strncmp(p, "HTTP/1.", 7) != 0) {
        return 0;
    }

    p += 9;    /* skip "HTTP/1.x " and land on the 3-digit status code */

    if (p[0] < '0' || p[0] > '9'
        || p[1] < '0' || p[1] > '9'
        || p[2] < '0' || p[2] > '9')
    {
        return 0;
    }

    status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');

    if (hscf->match) {
        status_min = hscf->match->status_min;
        status_max = hscf->match->status_max;
    } else {
        status_min = 200;
        status_max = 399;
    }

    if (status < status_min || status > status_max) {
        return 0;
    }

    if (hscf->match && hscf->match->body_regex) {

        body = (u_char *) ngx_strstr(peer->recv, "\r\n\r\n");
        if (body == NULL) {
            /* headers not even complete: nothing to match against */
            return 0;
        }

        body += 4;

        body_str.data = body;
        body_str.len  = (peer->recv + peer->recv_len) - body;

        if (ngx_regex_exec(hscf->match->body_regex, &body_str,
                           captures, 3)
            < 0)
        {
            return 0;
        }
    }

    return 1;
}


/*
 * Watches a connection kept open between probe cycles. Any activity on it
 * while idle — unsolicited data, EOF, or an error — means it can no longer
 * be reused, so it is closed and the peer falls back to a fresh connect on
 * its next probe.
 */
static void
ngx_http_hc_idle_closed_handler(ngx_event_t *ev)
{
    ngx_connection_t    *c    = ev->data;
    ngx_http_hc_peer_t  *peer = c->data;
    u_char                buf[1];
    ssize_t               n;
    ngx_pool_t            *pool;

    n = c->recv(c, buf, 1);

    if (n == NGX_AGAIN) {
        return;
    }

    peer->conn_open = 0;

#if (NGX_SSL)
    if (c->ssl) {
        c->ssl->no_wait_shutdown = 1;
        c->ssl->no_send_shutdown = 1;
        (void) ngx_ssl_shutdown(c);
    }
#endif

    pool = c->pool;

    ngx_close_connection(c);

    if (pool) {
        ngx_destroy_pool(pool);
    }

    peer->pc.connection = NULL;
}


static void
ngx_http_hc_finish_check(ngx_http_hc_peer_t *peer, ngx_int_t success)
{
    ngx_http_hc_main_conf_t  *hmcf;
    ngx_http_hc_shm_peer_t   *shm;
    ngx_connection_t         *c;
    ngx_pool_t               *pool;
    ngx_flag_t                 keep;

    keep = peer->conf->keepalive && peer->keepalive_ok && peer->pc.connection;

    if (keep) {
        c = peer->pc.connection;

        if (c->read->timer_set) {
            ngx_del_timer(c->read);
        }
        if (c->write->timer_set) {
            ngx_del_timer(c->write);
        }

        c->read->handler = ngx_http_hc_idle_closed_handler;

        if (ngx_handle_read_event(c->read, 0) == NGX_OK) {
            peer->conn_open = 1;

        } else {
            /* couldn't arm the idle watcher: fall back to closing */
            keep = 0;
        }
    }

    if (!keep && peer->pc.connection) {
        c = peer->pc.connection;

        if (c->read->timer_set) {
            ngx_del_timer(c->read);
        }
        if (c->write->timer_set) {
            ngx_del_timer(c->write);
        }

#if (NGX_SSL)
        if (c->ssl) {

            if (c->ssl->handshaked) {
                /* cache the session for reuse on the next probe cycle;
                 * done here (after the response was read), not right
                 * after the handshake, so that a TLS1.3 post-handshake
                 * NewSessionTicket has had a chance to arrive */

                ngx_ssl_session_t  *new_session;

                new_session = SSL_get1_session(c->ssl->connection);

                if (new_session) {
                    if (peer->ssl_session) {
                        SSL_SESSION_free(peer->ssl_session);
                    }
                    peer->ssl_session = new_session;
                }
            }

            c->ssl->no_wait_shutdown = 1;
            c->ssl->no_send_shutdown = 1;
            (void) ngx_ssl_shutdown(c);
        }
#endif

        pool = c->pool;

        ngx_close_connection(c);

        if (pool) {
            ngx_destroy_pool(pool);
        }

        peer->pc.connection = NULL;
    }

    peer->busy         = 0;
    peer->keepalive_ok = 0;

    hmcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
               ngx_http_upstream_healthcheck_module);

    if (hmcf == NULL || hmcf->shm_peers == NULL) {
        return;
    }

    shm = &hmcf->shm_peers[peer->index];

    ngx_rwlock_wlock(&shm->lock);

    if (success) {
        shm->fall_count = 0;

        if (shm->total_fails > 0) {
            shm->last_fails  = shm->total_fails;
            shm->total_fails = 0;
        }

        if (shm->down) {
            shm->rise_count++;

            if (shm->rise_count >= peer->conf->rise) {
                shm->down        = 0;
                shm->rise_count  = 0;
                shm->last_change = ngx_time();

                ngx_log_error(NGX_LOG_WARN, peer->check_ev.log, 0,
                              "healthcheck: peer %V (%V) is UP",
                              &peer->name, &peer->upstream);
            }
        }

    } else {
        shm->total_fails++;
        shm->rise_count = 0;

        if (!shm->down) {
            shm->fall_count++;

            if (shm->fall_count >= peer->conf->fall) {
                shm->down        = 1;
                shm->fall_count  = 0;
                shm->last_change = ngx_time();

                ngx_log_error(NGX_LOG_WARN, peer->check_ev.log, 0,
                              "healthcheck: peer %V (%V) is DOWN",
                              &peer->name, &peer->upstream);
            }
        }
    }

    ngx_rwlock_unlock(&shm->lock);
}
