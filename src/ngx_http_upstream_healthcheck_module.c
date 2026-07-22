
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
 *  - ngx_http_upstream_healthcheck_module.c  module registration, directive
 *                                             parsing (this file)
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
static char *ngx_http_hc_healthcheck(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_status(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_shm_size(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_resolver(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_match_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_match_directive(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);


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


/* ---------------------------------------------------- "healthcheck" cmd */

static char *
ngx_http_hc_healthcheck(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_hc_srv_conf_t        *hscf = conf;
    ngx_http_hc_main_conf_t       *hmcf;
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t                     *value, s, name;
    ngx_uint_t                     i;
    ngx_int_t                      n;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            s.data = value[i].data + 9;
            s.len  = value[i].len - 9;
            n = ngx_atoi(s.data, s.len);
            if (n == NGX_ERROR || n <= 0) {
                goto invalid;
            }
            hscf->interval = (ngx_msec_t) n;
            continue;
        }

        if (ngx_strncmp(value[i].data, "timeout=", 8) == 0) {
            s.data = value[i].data + 8;
            s.len  = value[i].len - 8;
            n = ngx_atoi(s.data, s.len);
            if (n == NGX_ERROR || n <= 0) {
                goto invalid;
            }
            hscf->timeout = (ngx_msec_t) n;
            continue;
        }

        if (ngx_strncmp(value[i].data, "fall=", 5) == 0) {
            s.data = value[i].data + 5;
            s.len  = value[i].len - 5;
            n = ngx_atoi(s.data, s.len);
            if (n == NGX_ERROR || n <= 0) {
                goto invalid;
            }
            hscf->fall = (ngx_uint_t) n;
            continue;
        }

        if (ngx_strncmp(value[i].data, "rise=", 5) == 0) {
            s.data = value[i].data + 5;
            s.len  = value[i].len - 5;
            n = ngx_atoi(s.data, s.len);
            if (n == NGX_ERROR || n <= 0) {
                goto invalid;
            }
            hscf->rise = (ngx_uint_t) n;
            continue;
        }

        if (ngx_strcmp(value[i].data, "keepalive") == 0) {
            hscf->keepalive = 1;
            continue;
        }

        if (ngx_strcmp(value[i].data, "resolve") == 0) {
            hscf->resolve = 1;
            continue;
        }

        if (ngx_strncmp(value[i].data, "resolve_interval=", 17) == 0) {
            s.data = value[i].data + 17;
            s.len  = value[i].len - 17;
            n = ngx_atoi(s.data, s.len);
            if (n == NGX_ERROR || n <= 0) {
                goto invalid;
            }
            hscf->resolve_interval = (ngx_msec_t) n;
            continue;
        }

        if (ngx_strncmp(value[i].data, "uri=", 4) == 0) {
            hscf->uri.data = value[i].data + 4;
            hscf->uri.len  = value[i].len - 4;
            if (hscf->uri.len == 0 || hscf->uri.data[0] != '/') {
                goto invalid;
            }
            continue;
        }

        if (ngx_strncmp(value[i].data, "match=", 6) == 0) {
            hscf->match_name.data = value[i].data + 6;
            hscf->match_name.len  = value[i].len - 6;
            if (hscf->match_name.len == 0) {
                goto invalid;
            }
            continue;
        }

        if (ngx_strcmp(value[i].data, "ssl") == 0) {
#if (NGX_SSL)
            hscf->ssl = 1;
            continue;
#else
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "healthcheck: \"ssl\" requires nginx built with "
                "--with-http_ssl_module");
            return NGX_CONF_ERROR;
#endif
        }

        if (ngx_strncmp(value[i].data, "ssl_verify=", 11) == 0) {
#if (NGX_SSL)
            s.data = value[i].data + 11;
            s.len  = value[i].len - 11;

            if (s.len == 2 && ngx_strncmp(s.data, "on", 2) == 0) {
                hscf->ssl_verify = 1;

            } else if (s.len == 3 && ngx_strncmp(s.data, "off", 3) == 0) {
                hscf->ssl_verify = 0;

            } else {
                goto invalid;
            }
            continue;
#else
            goto ssl_not_compiled;
#endif
        }

        if (ngx_strncmp(value[i].data, "ssl_name=", 9) == 0) {
#if (NGX_SSL)
            hscf->ssl_name.data = value[i].data + 9;
            hscf->ssl_name.len  = value[i].len - 9;
            continue;
#else
            goto ssl_not_compiled;
#endif
        }

        if (ngx_strncmp(value[i].data, "ssl_trusted_certificate=", 24) == 0) {
#if (NGX_SSL)
            hscf->ssl_trusted_certificate.data = value[i].data + 24;
            hscf->ssl_trusted_certificate.len  = value[i].len - 24;
            continue;
#else
            goto ssl_not_compiled;
#endif
        }

invalid:
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "healthcheck: invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;

#if !(NGX_SSL)
ssl_not_compiled:
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "healthcheck: \"%V\" requires nginx built with "
            "--with-http_ssl_module", &value[i]);
        return NGX_CONF_ERROR;
#endif
    }

    hscf->enabled = 1;

    /* register the shared zone once */

    hmcf = ngx_http_conf_get_module_main_conf(cf,
               ngx_http_upstream_healthcheck_module);

    if (hmcf->shm_zone == NULL) {
        ngx_str_set(&name, NGX_HC_SHM_NAME);

        hmcf->shm_zone = ngx_shared_memory_add(cf, &name,
                             hmcf->shm_size ? hmcf->shm_size : NGX_HC_SHM_SIZE,
                             &ngx_http_upstream_healthcheck_module);
        if (hmcf->shm_zone == NULL) {
            return NGX_CONF_ERROR;
        }

        hmcf->shm_zone->init = ngx_http_hc_init_shm_zone;
        hmcf->shm_zone->data = hmcf;
    }

    /* hook the upstream initialization of this upstream block */

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    hscf->original_init_upstream = uscf->peer.init_upstream
                                   ? uscf->peer.init_upstream
                                   : ngx_http_upstream_init_round_robin;

    uscf->peer.init_upstream = ngx_http_hc_init_upstream;

    return NGX_CONF_OK;
}


static char *
ngx_http_hc_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_hc_status_handler;

    return NGX_CONF_OK;
}


/* --------------------------------------------- "healthcheck_shm_size" */

static char *
ngx_http_hc_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_hc_main_conf_t  *hmcf = conf;
    ngx_str_t                *value;
    ssize_t                    size;

    if (hmcf->shm_zone != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "healthcheck_shm_size must appear before any \"healthcheck\" "
            "directive (the shared zone is already created)");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    size = ngx_parse_size(&value[1]);
    if (size == NGX_ERROR || size < (ssize_t) (8 * 1024)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid healthcheck_shm_size value \"%V\" "
                           "(minimum 8k)", &value[1]);
        return NGX_CONF_ERROR;
    }

    hmcf->shm_size = (size_t) size;

    return NGX_CONF_OK;
}


/* ---------------------------------------------- "healthcheck_resolver" */

static char *
ngx_http_hc_resolver(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_hc_main_conf_t  *hmcf = conf;
    ngx_str_t                *value;

    if (hmcf->resolver != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "healthcheck_resolver is duplicate");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    hmcf->resolver = ngx_resolver_create(cf, &value[1], cf->args->nelts - 1);
    if (hmcf->resolver == NULL) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/* ------------------------------------------------- "healthcheck_match" */

static char *
ngx_http_hc_match_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_hc_main_conf_t  *hmcf;
    ngx_http_hc_match_t      *match, *m;
    ngx_str_t                *value;
    ngx_conf_t                save;
    ngx_uint_t                i;
    char                     *rv;

    value = cf->args->elts;

    hmcf = ngx_http_conf_get_module_main_conf(cf,
               ngx_http_upstream_healthcheck_module);

    m = hmcf->matches.elts;
    for (i = 0; i < hmcf->matches.nelts; i++) {
        if (m[i].name.len == value[1].len
            && ngx_strncmp(m[i].name.data, value[1].data, value[1].len) == 0)
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "healthcheck_match \"%V\" already defined",
                               &value[1]);
            return NGX_CONF_ERROR;
        }
    }

    match = ngx_array_push(&hmcf->matches);
    if (match == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(match, sizeof(ngx_http_hc_match_t));
    match->name       = value[1];
    match->status_min = 200;
    match->status_max = 399;

    save = *cf;
    cf->handler      = ngx_http_hc_match_directive;
    cf->handler_conf = (void *) match;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    return rv;
}


static char *
ngx_http_hc_match_directive(ngx_conf_t *cf, ngx_command_t *dummy, void *conf)
{
    ngx_http_hc_match_t   *match;
    ngx_str_t             *value, s;
    ngx_regex_compile_t    rc;
    u_char                 errstr[NGX_MAX_CONF_ERRSTR];
    u_char                *dash;
    ngx_int_t               n;

    match = (ngx_http_hc_match_t *) cf->handler_conf;
    value = cf->args->elts;

    if (ngx_strcmp(value[0].data, "status") == 0) {

        if (cf->args->nelts != 2) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"status\" takes exactly one parameter");
            return NGX_CONF_ERROR;
        }

        dash = (u_char *) ngx_strlchr(value[1].data,
                              value[1].data + value[1].len, '-');

        if (dash) {
            s.data = value[1].data;
            s.len  = dash - value[1].data;
            n = ngx_atoi(s.data, s.len);
            if (n == NGX_ERROR) {
                goto invalid_status;
            }
            match->status_min = (ngx_uint_t) n;

            s.data = dash + 1;
            s.len  = value[1].data + value[1].len - (dash + 1);
            n = ngx_atoi(s.data, s.len);
            if (n == NGX_ERROR) {
                goto invalid_status;
            }
            match->status_max = (ngx_uint_t) n;

        } else {
            n = ngx_atoi(value[1].data, value[1].len);
            if (n == NGX_ERROR) {
                goto invalid_status;
            }
            match->status_min = (ngx_uint_t) n;
            match->status_max = (ngx_uint_t) n;
        }

        if (match->status_min > match->status_max) {
            goto invalid_status;
        }

        return NGX_CONF_OK;

invalid_status:
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid status \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (ngx_strcmp(value[0].data, "body") == 0) {

        if (cf->args->nelts != 3 || ngx_strcmp(value[1].data, "~") != 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "syntax: body ~ \"regex\";");
            return NGX_CONF_ERROR;
        }

        ngx_memzero(&rc, sizeof(ngx_regex_compile_t));

        rc.pattern = value[2];
        rc.pool    = cf->pool;
        rc.err.len = NGX_MAX_CONF_ERRSTR;
        rc.err.data = errstr;

        if (ngx_regex_compile(&rc) != NGX_OK) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid regex \"%V\": %V",
                               &value[2], &rc.err);
            return NGX_CONF_ERROR;
        }

        match->body_regex   = rc.regex;
        match->body_pattern = value[2];

        return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "unknown directive \"%V\" in healthcheck_match",
                       &value[0]);
    return NGX_CONF_ERROR;
}
