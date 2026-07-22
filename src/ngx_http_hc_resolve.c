
/*
 * ngx_http_hc_resolve.c — periodic DNS re-resolution for peers configured
 * with a hostname ("resolve"). Updates the peer's live socket address in
 * place; see the module README for the "zone" requirement this relies on.
 */

#include "ngx_http_upstream_healthcheck_module.h"


static ngx_int_t ngx_http_hc_update_sockaddr(struct sockaddr *dst,
    socklen_t dst_len, struct sockaddr *src, socklen_t src_len);
static void ngx_http_hc_resolve_handler(ngx_resolver_ctx_t *ctx);


/*
 * In-place update of an existing peer's socket address once "resolve"
 * finds a new one, instead of swapping pointers: since (with the "zone"
 * requirement enforced at config time) the sockaddr memory lives in the
 * upstream's shared zone, all workers already point at the very same
 * bytes — overwriting them under the peer lock is all that's needed for
 * the change to be visible everywhere, immediately.
 *
 * Deliberately conservative: a family/size mismatch (e.g. an AAAA answer
 * for a peer configured as IPv4) is left alone rather than reallocated,
 * since the existing buffer is sized for the original family and
 * swapping pointers would reintroduce exactly the cross-worker
 * visibility problem "zone" is meant to solve.
 */
static ngx_int_t
ngx_http_hc_update_sockaddr(struct sockaddr *dst, socklen_t dst_len,
    struct sockaddr *src, socklen_t src_len)
{
    if (dst->sa_family != src->sa_family || dst_len != src_len) {
        return NGX_DECLINED;
    }

    switch (dst->sa_family) {

    case AF_INET:
        {
        struct sockaddr_in  *d = (struct sockaddr_in *) dst;
        struct sockaddr_in  *s = (struct sockaddr_in *) src;

        if (d->sin_addr.s_addr == s->sin_addr.s_addr) {
            return NGX_DECLINED;
        }

        d->sin_addr = s->sin_addr;
        return NGX_OK;
        }

#if (NGX_HAVE_INET6)
    case AF_INET6:
        {
        struct sockaddr_in6  *d = (struct sockaddr_in6 *) dst;
        struct sockaddr_in6  *s = (struct sockaddr_in6 *) src;

        if (ngx_memcmp(&d->sin6_addr, &s->sin6_addr, 16) == 0) {
            return NGX_DECLINED;
        }

        d->sin6_addr = s->sin6_addr;
        return NGX_OK;
        }
#endif

    default:
        return NGX_DECLINED;
    }
}


void
ngx_http_hc_resolve_begin(ngx_event_t *ev)
{
    ngx_http_hc_peer_t       *peer = ev->data;
    ngx_http_hc_main_conf_t  *hmcf;
    ngx_resolver_ctx_t       *ctx;

    ngx_add_timer(ev, peer->conf->resolve_interval);

    if (peer->resolve_ctx) {
        /* previous resolution still in flight (slow/unresponsive
         * resolver): skip this round rather than pile up requests */
        return;
    }

    hmcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
               ngx_http_upstream_healthcheck_module);

    if (hmcf == NULL || hmcf->resolver == NULL) {
        return;
    }

    ctx = ngx_resolve_start(hmcf->resolver, NULL);
    if (ctx == NULL || ctx == NGX_NO_RESOLVER) {
        return;
    }

    ctx->name    = peer->hostname;
    ctx->handler = ngx_http_hc_resolve_handler;
    ctx->data    = peer;
    ctx->timeout = 30000;

    peer->resolve_ctx = ctx;

    if (ngx_resolve_name(ctx) != NGX_OK) {
        peer->resolve_ctx = NULL;
    }
}


static void
ngx_http_hc_resolve_handler(ngx_resolver_ctx_t *ctx)
{
    ngx_http_hc_peer_t  *peer = ctx->data;
    ngx_uint_t            i;
    ngx_flag_t            matched;

    if (ctx->state) {
        ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
            "healthcheck: resolving \"%V\" failed (%s), keeping the last "
            "known address for %V (%V)",
            &ctx->name, ngx_resolver_strerror(ctx->state),
            &peer->name, &peer->upstream);

        ngx_resolve_name_done(ctx);
        peer->resolve_ctx = NULL;
        return;
    }

    matched = 0;

    for (i = 0; i < ctx->naddrs; i++) {
        struct sockaddr_storage  tmp;
        socklen_t                tmp_len;

        tmp_len = ctx->addrs[i].socklen;

        if (ctx->addrs[i].sockaddr->sa_family != peer->sockaddr->sa_family
            || tmp_len > sizeof(tmp))
        {
            continue;
        }

        ngx_memcpy(&tmp, ctx->addrs[i].sockaddr, tmp_len);

        switch (((struct sockaddr *) &tmp)->sa_family) {
        case AF_INET:
            ((struct sockaddr_in *) &tmp)->sin_port = htons(peer->port);
            break;
#if (NGX_HAVE_INET6)
        case AF_INET6:
            ((struct sockaddr_in6 *) &tmp)->sin6_port = htons(peer->port);
            break;
#endif
        default:
            continue;
        }

        matched = 1;

        ngx_http_upstream_rr_peers_rlock(peer->rr_peers);
        ngx_http_upstream_rr_peer_lock(peer->rr_peers, peer->rr_peer);

        if (ngx_http_hc_update_sockaddr(peer->sockaddr, peer->socklen,
                                        (struct sockaddr *) &tmp, tmp_len)
            == NGX_OK)
        {
            ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                "healthcheck: peer %V (%V) resolved to a new address "
                "for \"%V\"", &peer->name, &peer->upstream, &ctx->name);
        }

        ngx_http_upstream_rr_peer_unlock(peer->rr_peers, peer->rr_peer);
        ngx_http_upstream_rr_peers_unlock(peer->rr_peers);

        break;
    }

    if (!matched) {
        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
            "healthcheck: resolving \"%V\" returned no addresses "
            "compatible with the current family of %V (%V)",
            &ctx->name, &peer->name, &peer->upstream);
    }

    ngx_resolve_name_done(ctx);
    peer->resolve_ctx = NULL;
}
