
/*
 * ngx_http_hc_status.c — the healthcheck_status plain-text status page.
 */

#include "ngx_http_upstream_healthcheck_module.h"


ngx_int_t
ngx_http_hc_status_handler(ngx_http_request_t *r)
{
    ngx_int_t                 rc;
    ngx_buf_t                *b;
    ngx_uint_t                i;
    ngx_chain_t               out;
    ngx_http_hc_peer_t       *peers;
    ngx_http_hc_shm_peer_t   *shm;
    ngx_http_hc_main_conf_t  *hmcf;
    size_t                    size;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    hmcf = ngx_http_get_module_main_conf(r,
               ngx_http_upstream_healthcheck_module);

    size = 128 + hmcf->peers.nelts * 256;

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->last = ngx_sprintf(b->last,
                  "upstream healthcheck status\n"
                  "---------------------------\n");

    if (hmcf->shm_peers == NULL || hmcf->peers.nelts == 0) {
        b->last = ngx_sprintf(b->last, "no monitored peers\n");

    } else {
        peers = hmcf->peers.elts;

        for (i = 0; i < hmcf->peers.nelts; i++) {
            ngx_uint_t  down, fails, last_fails;

            shm = &hmcf->shm_peers[peers[i].index];

            ngx_rwlock_rlock(&shm->lock);
            down       = shm->down;
            fails      = shm->total_fails;
            last_fails = shm->last_fails;
            ngx_rwlock_unlock(&shm->lock);

            b->last = ngx_sprintf(b->last,
                    "%V  peer=%V  status=%s  fails=%ui  last_fails=%ui\n",
                    &peers[i].upstream, &peers[i].name,
                    down ? "DOWN" : "up", fails, last_fails);
        }
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;
    ngx_str_set(&r->headers_out.content_type, "text/plain");

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b->last_buf     = 1;
    b->last_in_chain = 1;

    out.buf  = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}
