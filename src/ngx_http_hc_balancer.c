
/*
 * ngx_http_hc_balancer.c — interception of the underlying load balancer's
 * peer.init / peer.get / peer.free (and, for https:// upstreams,
 * peer.set_session / peer.save_session) so unhealthy peers are skipped at
 * request time. No patch to nginx's own balancer modules is required.
 */

#include "ngx_http_upstream_healthcheck_module.h"


static ngx_int_t ngx_http_hc_peer_is_down(ngx_http_hc_main_conf_t *hmcf,
    ngx_http_hc_srv_conf_t *hscf, struct sockaddr *sockaddr,
    socklen_t socklen);
static ngx_int_t ngx_http_hc_get_peer(ngx_peer_connection_t *pc, void *data);
static void ngx_http_hc_free_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state);
#if (NGX_SSL)
static ngx_int_t ngx_http_hc_set_session(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_hc_save_session(ngx_peer_connection_t *pc, void *data);
#endif


ngx_int_t
ngx_http_hc_init_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_hc_srv_conf_t      *hscf;
    ngx_http_hc_balancer_ctx_t  *ctx;

    hscf = ngx_http_conf_upstream_srv_conf(us,
               ngx_http_upstream_healthcheck_module);

    if (hscf->original_init_peer(r, us) != NGX_OK) {
        return NGX_ERROR;
    }

    ctx = ngx_palloc(r->pool, sizeof(ngx_http_hc_balancer_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->data          = r->upstream->peer.data;
    ctx->original_get  = r->upstream->peer.get;
    ctx->original_free = r->upstream->peer.free;
    ctx->hmcf          = ngx_http_get_module_main_conf(r,
                             ngx_http_upstream_healthcheck_module);
    ctx->hscf          = hscf;
    ctx->log           = r->connection->log;

    r->upstream->peer.data = ctx;
    r->upstream->peer.get  = ngx_http_hc_get_peer;
    r->upstream->peer.free = ngx_http_hc_free_peer;

#if (NGX_SSL)
    /*
     * proxy_pass to an https:// upstream calls peer.set_session/
     * save_session (TLS session reuse across requests) with peer.data,
     * expecting the original round-robin rrp there. Since peer.data now
     * points at ctx, those callbacks (still the original round-robin
     * ones, untouched below) would misinterpret ctx as an rrp and read
     * garbage — wrap them too so they get the real rrp back.
     */
    ctx->original_set_session  = r->upstream->peer.set_session;
    ctx->original_save_session = r->upstream->peer.save_session;

    if (ctx->original_set_session) {
        r->upstream->peer.set_session = ngx_http_hc_set_session;
    }

    if (ctx->original_save_session) {
        r->upstream->peer.save_session = ngx_http_hc_save_session;
    }
#endif

    return NGX_OK;
}


#if (NGX_SSL)

static ngx_int_t
ngx_http_hc_set_session(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_hc_balancer_ctx_t  *ctx = data;

    return ctx->original_set_session(pc, ctx->data);
}


static void
ngx_http_hc_save_session(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_hc_balancer_ctx_t  *ctx = data;

    ctx->original_save_session(pc, ctx->data);
}

#endif


static ngx_int_t
ngx_http_hc_peer_is_down(ngx_http_hc_main_conf_t *hmcf,
    ngx_http_hc_srv_conf_t *hscf, struct sockaddr *sockaddr, socklen_t socklen)
{
    ngx_uint_t                i, down;
    ngx_http_hc_peer_t       *peers;
    ngx_http_hc_shm_peer_t   *shm;

    if (hmcf == NULL || hmcf->shm_peers == NULL) {
        return 0;
    }

    peers = hmcf->peers.elts;

    for (i = 0; i < hmcf->peers.nelts; i++) {

        /* scope the match to this upstream: two different upstream{}
         * blocks may list the same ip:port without sharing health state */

        if (peers[i].conf != hscf) {
            continue;
        }

        if (ngx_cmp_sockaddr(peers[i].sockaddr, peers[i].socklen,
                             sockaddr, socklen, 1)
            == NGX_OK)
        {
            shm = &hmcf->shm_peers[peers[i].index];

            ngx_rwlock_rlock(&shm->lock);
            down = shm->down;
            ngx_rwlock_unlock(&shm->lock);

            return down ? 1 : 0;
        }
    }

    return 0;
}


static ngx_int_t
ngx_http_hc_get_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_hc_balancer_ctx_t  *ctx = data;
    ngx_int_t                    rc;
    ngx_uint_t                   i, max;

    max = ctx->hmcf->peers.nelts + 1;

    for (i = 0; i < max; i++) {

        rc = ctx->original_get(pc, ctx->data);

        if (rc != NGX_OK) {
            return rc;
        }

        if (!ngx_http_hc_peer_is_down(ctx->hmcf, ctx->hscf, pc->sockaddr,
                                      pc->socklen))
        {
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_INFO, ctx->log, 0,
                      "healthcheck: skipping unhealthy peer %V", pc->name);

        if (pc->tries <= 1) {
            return NGX_BUSY;
        }

        ctx->original_free(pc, ctx->data, NGX_PEER_FAILED);
    }

    return NGX_BUSY;
}


static void
ngx_http_hc_free_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
    ngx_http_hc_balancer_ctx_t  *ctx = data;

    ctx->original_free(pc, ctx->data, state);
}
