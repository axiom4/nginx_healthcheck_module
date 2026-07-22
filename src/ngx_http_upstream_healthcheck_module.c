
/*
 * ngx_http_upstream_healthcheck_module
 *
 * Active health checks for nginx open source.
 *
 * Usage:
 *
 *   http {
 *       healthcheck_match ok {
 *           status 200-399;
 *           body ~ "\"status\"\s*:\s*\"ok\"";
 *       }
 *
 *       upstream backend {
 *           server 10.0.0.1:8443;
 *           server 10.0.0.2:8443;
 *           healthcheck interval=5000 timeout=2000 fall=3 rise=2
 *                       uri=/health match=ok
 *                       ssl ssl_verify=off ssl_name=backend.internal;
 *       }
 *   }
 *
 *   server {
 *       location /hc-status {
 *           healthcheck_status;
 *       }
 *   }
 *
 * Design notes:
 *  - Probes run only in worker 0 (ngx_worker == 0) to avoid duplicated
 *    traffic; the resulting peer state lives in a shared memory zone so
 *    every worker sees it.
 *  - Peer selection is intercepted by wrapping peer.init / peer.get /
 *    peer.free of the underlying load balancing module (round robin,
 *    least_conn, ...). No patch to the nginx source is required.
 *  - A probe is a plain HTTP GET; by default any 2xx/3xx status line
 *    counts as success. A "healthcheck_match" block can require a
 *    narrower status range and/or a regex match against the body.
 *  - "ssl" wraps the probe connection in TLS (plain nginx SSL client
 *    connection, same mechanism used by proxy_pass to an https upstream).
 *
 * Source layout:
 *  - ngx_http_upstream_healthcheck_module.c  module registration (this file)
 *  - ngx_http_hc_config.c                     directive parsing
 *                                             ("healthcheck", "healthcheck_
 *                                             status", "healthcheck_match",
 *                                             "healthcheck_shm_size",
 *                                             "healthcheck_resolver")
 *  - ngx_http_hc_upstream.c                   peer collection at upstream
 *                                             init time, shm zone init
 *  - ngx_http_hc_balancer.c                   peer.init/get/free/set_session/
 *                                             save_session interception
 *  - ngx_http_hc_probe.c                      the active check state machine
 *                                             (connect, ssl, send, recv,
 *                                             match, finish)
 *  - ngx_http_hc_resolve.c                    periodic DNS re-resolution
 *                                             ("resolve")
 *  - ngx_http_hc_status.c                     the healthcheck_status page
 */

#include "ngx_http_upstream_healthcheck_module.h"


static void *ngx_http_hc_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_hc_create_srv_conf(ngx_conf_t *cf);


/* ------------------------------------------------------------ directives */

static ngx_command_t ngx_http_hc_commands[] = {

    { ngx_string("healthcheck"),
      NGX_HTTP_UPS_CONF|NGX_CONF_ANY,
      ngx_http_hc_healthcheck,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("healthcheck_status"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_hc_status,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("healthcheck_match"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
      ngx_http_hc_match_block,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("healthcheck_shm_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_hc_shm_size,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("healthcheck_resolver"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_1MORE,
      ngx_http_hc_resolver,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t ngx_http_hc_module_ctx = {
    NULL,                               /* preconfiguration  */
    NULL,                               /* postconfiguration */

    ngx_http_hc_create_main_conf,       /* create main conf  */
    NULL,                               /* init main conf    */

    ngx_http_hc_create_srv_conf,        /* create srv conf   */
    NULL,                               /* merge srv conf    */

    NULL,                               /* create loc conf   */
    NULL                                /* merge loc conf    */
};


ngx_module_t ngx_http_upstream_healthcheck_module = {
    NGX_MODULE_V1,
    &ngx_http_hc_module_ctx,
    ngx_http_hc_commands,
    NGX_HTTP_MODULE,
    NULL,                               /* init master     */
    NULL,                               /* init module     */
    ngx_http_hc_init_process,           /* init process    */
    NULL,                               /* init thread     */
    NULL,                               /* exit thread     */
    NULL,                               /* exit process    */
    NULL,                               /* exit master     */
    NGX_MODULE_V1_PADDING
};


/* --------------------------------------------------------- conf create */

static void *
ngx_http_hc_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_hc_main_conf_t  *hmcf;

    hmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_hc_main_conf_t));
    if (hmcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&hmcf->peers, cf->pool, 16,
                       sizeof(ngx_http_hc_peer_t))
        != NGX_OK)
    {
        return NULL;
    }

    if (ngx_array_init(&hmcf->matches, cf->pool, 4,
                       sizeof(ngx_http_hc_match_t))
        != NGX_OK)
    {
        return NULL;
    }

    return hmcf;
}


static void *
ngx_http_hc_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_hc_srv_conf_t  *hscf;

    hscf = ngx_pcalloc(cf->pool, sizeof(ngx_http_hc_srv_conf_t));
    if (hscf == NULL) {
        return NULL;
    }

    hscf->enabled  = 0;
    hscf->interval = 5000;
    hscf->timeout  = 2000;
    hscf->fall     = 3;
    hscf->rise     = 2;
    ngx_str_set(&hscf->uri, "/");
    hscf->resolve_interval = 30000;

    return hscf;
}
