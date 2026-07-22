
/*
 * ngx_http_hc_upstream.c — collects the peers of an upstream{} block for
 * probing when the "healthcheck" directive is used, and initializes the
 * shared memory zone that holds their state.
 */

#include "ngx_http_upstream_healthcheck_module.h"


/*
 * Splits the raw text of a "server" directive argument (e.g.
 * "backend.example.com:8080" or "[::1]:8080") into a host part and a
 * numeric port, so a hostname can be handed to the resolver independently
 * of the port nginx already parsed for its own sockaddr. Returns NGX_ERROR
 * for anything that doesn't look like "host:port" (unix sockets, malformed
 * text) — such peers are simply left out of "resolve" tracking.
 */
static ngx_int_t
ngx_http_hc_parse_server_name(ngx_str_t *text, ngx_str_t *host,
    in_port_t *port)
{
    u_char     *p, *last, *colon, *close_bracket;
    ngx_int_t   n;

    if (text->len == 0) {
        return NGX_ERROR;
    }

    last = text->data + text->len;

    if (text->data[0] == '[') {
        close_bracket = ngx_strlchr(text->data, last, ']');

        if (close_bracket == NULL
            || close_bracket + 1 >= last
            || close_bracket[1] != ':')
        {
            return NGX_ERROR;
        }

        host->data = text->data + 1;
        host->len  = close_bracket - (text->data + 1);
        colon      = close_bracket + 1;

    } else {
        colon = NULL;

        for (p = text->data; p < last; p++) {
            if (*p == ':') {
                colon = p;   /* keep the last colon: host:port */
            }
        }

        if (colon == NULL) {
            return NGX_ERROR;
        }

        host->data = text->data;
        host->len  = colon - text->data;
    }

    if (host->len == 0) {
        return NGX_ERROR;
    }

    n = ngx_atoi(colon + 1, last - (colon + 1));
    if (n == NGX_ERROR || n < 1 || n > 65535) {
        return NGX_ERROR;
    }

    *port = (in_port_t) n;

    return NGX_OK;
}


/* --------------------------------------------------- upstream init hook */

ngx_int_t
ngx_http_hc_init_upstream(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_uint_t                     i;
    ngx_http_hc_peer_t            *peer;
    ngx_http_hc_srv_conf_t        *hscf;
    ngx_http_hc_main_conf_t       *hmcf;
    ngx_http_upstream_rr_peer_t   *rr_peer;
    ngx_http_upstream_rr_peers_t  *rr_peers;

    hscf = ngx_http_conf_upstream_srv_conf(us,
               ngx_http_upstream_healthcheck_module);

    if (hscf->original_init_upstream(cf, us) != NGX_OK) {
        return NGX_ERROR;
    }

    /* wrap per-request peer init */
    hscf->original_init_peer = us->peer.init;
    us->peer.init = ngx_http_hc_init_peer;

    hmcf = ngx_http_conf_get_module_main_conf(cf,
               ngx_http_upstream_healthcheck_module);

    /* resolve match=NAME to the parsed healthcheck_match block, if any */

    if (hscf->match_name.len) {
        ngx_http_hc_match_t  *matches;
        ngx_uint_t            j;
        ngx_flag_t            found;

        matches = hmcf->matches.elts;
        found   = 0;

        for (j = 0; j < hmcf->matches.nelts; j++) {
            if (matches[j].name.len == hscf->match_name.len
                && ngx_strncmp(matches[j].name.data, hscf->match_name.data,
                              matches[j].name.len)
                   == 0)
            {
                hscf->match = &matches[j];
                found = 1;
                break;
            }
        }

        if (!found) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                "healthcheck: match \"%V\" not defined (use "
                "healthcheck_match)", &hscf->match_name);
            return NGX_ERROR;
        }
    }

#if (NGX_SSL)
    if (hscf->ssl) {
        hscf->ssl_ctx = ngx_pcalloc(cf->pool, sizeof(ngx_ssl_t));
        if (hscf->ssl_ctx == NULL) {
            return NGX_ERROR;
        }

        hscf->ssl_ctx->log = cf->log;

        if (ngx_ssl_create(hscf->ssl_ctx,
                           NGX_SSL_TLSv1|NGX_SSL_TLSv1_1
                           |NGX_SSL_TLSv1_2|NGX_SSL_TLSv1_3, NULL)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        if (hscf->ssl_verify) {
            if (hscf->ssl_trusted_certificate.len == 0) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                    "healthcheck: ssl_verify=on requires "
                    "ssl_trusted_certificate=<path>");
                return NGX_ERROR;
            }

            if (ngx_ssl_trusted_certificate(cf, hscf->ssl_ctx,
                                            &hscf->ssl_trusted_certificate, 1)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            SSL_CTX_set_verify(hscf->ssl_ctx->ctx, SSL_VERIFY_PEER, NULL);
        }
    }
#endif

    if (hscf->resolve) {
        if (hmcf->resolver == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                "healthcheck: \"resolve\" requires \"healthcheck_resolver\" "
                "configured in the http context");
            return NGX_ERROR;
        }

#if (NGX_HTTP_UPSTREAM_ZONE)
        /*
         * us->shm_zone is set synchronously by the "zone" directive at
         * parse time. The actual shared slab (rr_peers->shpool) isn't
         * mapped until later, when nginx initializes shared zones after
         * all config parsing is done — too late to check here, but its
         * presence is exactly what us->shm_zone being non-NULL promises.
         */
        if (us->shm_zone == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                "healthcheck: \"resolve\" requires the \"zone\" directive "
                "on this upstream (otherwise the updated address wouldn't "
                "be visible to the other workers)");
            return NGX_ERROR;
        }
#else
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
            "healthcheck: \"resolve\" requires nginx built with upstream "
            "zone support");
        return NGX_ERROR;
#endif
    }

    /* collect the peers of this upstream for probing */

    rr_peers = us->peer.data;

    for (rr_peer = rr_peers->peer, i = 0;
         rr_peer != NULL;
         rr_peer = rr_peer->next, i++)
    {
        peer = ngx_array_push(&hmcf->peers);
        if (peer == NULL) {
            return NGX_ERROR;
        }

        ngx_memzero(peer, sizeof(ngx_http_hc_peer_t));

        peer->index    = hmcf->peers.nelts - 1;
        peer->sockaddr = rr_peer->sockaddr;
        peer->socklen  = rr_peer->socklen;
        peer->name     = rr_peer->name;
        peer->upstream = us->host;
        peer->conf     = hscf;
        peer->rr_peers = rr_peers;
        peer->rr_peer  = rr_peer;

        if (hscf->resolve) {
            ngx_str_t   host;
            in_port_t   port;
            u_char      ipv6_buf[16];
            ngx_flag_t  is_literal;

            if (ngx_http_hc_parse_server_name(&rr_peer->server, &host, &port)
                != NGX_OK)
            {
                ngx_log_error(NGX_LOG_WARN, cf->log, 0,
                    "healthcheck: couldn't extract host/port from "
                    "\"%V\" for peer %V (%V), \"resolve\" has no effect "
                    "for this peer (unix socket? literal address without "
                    "an explicit port?)",
                    &rr_peer->server, &peer->name, &peer->upstream);

            } else {
                is_literal = ngx_inet_addr(host.data, host.len)
                                 != INADDR_NONE;

#if (NGX_HAVE_INET6)
                if (!is_literal) {
                    is_literal = ngx_inet6_addr(host.data, host.len,
                                                ipv6_buf)
                                 == NGX_OK;
                }
#else
                (void) ipv6_buf;
#endif

                if (is_literal) {
                    ngx_log_error(NGX_LOG_INFO, cf->log, 0,
                        "healthcheck: peer %V (%V) has a literal address, "
                        "\"resolve\" has no effect for this peer",
                        &peer->name, &peer->upstream);

                } else {
                    /* a real hostname: track it for periodic re-resolution */
                    peer->hostname = host;
                    peer->port     = port;
                }
            }
        }
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_hc_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t          *shpool;
    ngx_http_hc_main_conf_t  *hmcf, *ohmcf;
    ngx_uint_t                n;

    hmcf  = shm_zone->data;
    ohmcf = data;                        /* old cycle conf, on reload */

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    n = hmcf->peers.nelts;
    if (n == 0) {
        return NGX_OK;
    }

    hmcf->shm_peers = ngx_slab_calloc(shpool,
                          n * sizeof(ngx_http_hc_shm_peer_t));
    if (hmcf->shm_peers == NULL) {
        return NGX_ERROR;
    }

    /* preserve state across reloads when the peer set did not change */

    if (ohmcf && ohmcf->shm_peers && ohmcf->peers.nelts == n) {
        ngx_memcpy(hmcf->shm_peers, ohmcf->shm_peers,
                   n * sizeof(ngx_http_hc_shm_peer_t));
    }

    return NGX_OK;
}
