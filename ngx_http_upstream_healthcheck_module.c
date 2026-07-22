
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
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HC_SHM_NAME  "upstream_healthcheck"
#define NGX_HC_SHM_SIZE  (128 * 1024)
#define NGX_HC_RECV_SIZE 1024


/* ---------------------------------------------------------------- types */

typedef struct {
    ngx_uint_t                          fall_count;
    ngx_uint_t                          rise_count;
    ngx_uint_t                          down;
    ngx_uint_t                          total_checks;
    ngx_uint_t                          total_fails;
    time_t                              last_change;
} ngx_http_hc_shm_peer_t;


/* "healthcheck_match NAME { status ...; body ~ ...; }" */
typedef struct {
    ngx_str_t                           name;
    ngx_uint_t                          status_min;
    ngx_uint_t                          status_max;
    ngx_regex_t                        *body_regex;
    ngx_str_t                           body_pattern;   /* for diagnostics */
} ngx_http_hc_match_t;


typedef struct {
    ngx_flag_t                          enabled;
    ngx_msec_t                          interval;
    ngx_msec_t                          timeout;
    ngx_uint_t                          fall;
    ngx_uint_t                          rise;
    ngx_str_t                           uri;

    ngx_str_t                           match_name;
    ngx_http_hc_match_t                *match;

#if (NGX_SSL)
    ngx_flag_t                          ssl;
    ngx_flag_t                          ssl_verify;
    ngx_str_t                           ssl_name;
    ngx_str_t                           ssl_trusted_certificate;
    ngx_ssl_t                          *ssl_ctx;
#endif

    ngx_http_upstream_init_pt           original_init_upstream;
    ngx_http_upstream_init_peer_pt      original_init_peer;
} ngx_http_hc_srv_conf_t;


typedef struct {
    ngx_uint_t                          index;      /* slot in shm array   */
    struct sockaddr                    *sockaddr;
    socklen_t                           socklen;
    ngx_str_t                           name;       /* "ip:port"           */
    ngx_str_t                           upstream;   /* upstream block name */
    ngx_http_hc_srv_conf_t             *conf;

    ngx_event_t                         check_ev;   /* periodic timer      */
    ngx_peer_connection_t               pc;
    ngx_buf_t                          *request;
    u_char                              recv[NGX_HC_RECV_SIZE];
    ngx_uint_t                          recv_len;
    unsigned                            busy:1;
} ngx_http_hc_peer_t;


typedef struct {
    ngx_array_t                         peers;      /* ngx_http_hc_peer_t  */
    ngx_array_t                         matches;    /* ngx_http_hc_match_t */
    ngx_shm_zone_t                     *shm_zone;
    ngx_http_hc_shm_peer_t             *shm_peers;  /* set at zone init    */
} ngx_http_hc_main_conf_t;


/* per-request wrapper around the original balancer */
typedef struct {
    void                               *data;
    ngx_event_get_peer_pt               original_get;
    ngx_event_free_peer_pt              original_free;
    ngx_http_hc_main_conf_t            *hmcf;
    ngx_log_t                          *log;
} ngx_http_hc_balancer_ctx_t;


/* ----------------------------------------------------------- prototypes */

static void *ngx_http_hc_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_hc_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_hc_healthcheck(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_status(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_match_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_hc_match_directive(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);
static ngx_int_t ngx_http_hc_init_shm_zone(ngx_shm_zone_t *shm_zone,
    void *data);

static ngx_int_t ngx_http_hc_init_upstream(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_hc_init_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_hc_get_peer(ngx_peer_connection_t *pc, void *data);
static void ngx_http_hc_free_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state);

static ngx_int_t ngx_http_hc_init_process(ngx_cycle_t *cycle);
static void ngx_http_hc_begin_check(ngx_event_t *ev);
static void ngx_http_hc_send_handler(ngx_event_t *ev);
static void ngx_http_hc_recv_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_hc_parse_response(ngx_http_hc_peer_t *peer);
static void ngx_http_hc_finish_check(ngx_http_hc_peer_t *peer,
    ngx_int_t success);
static ngx_int_t ngx_http_hc_dummy_get_peer(ngx_peer_connection_t *pc,
    void *data);
static ngx_int_t ngx_http_hc_status_handler(ngx_http_request_t *r);

#if (NGX_SSL)
static void ngx_http_hc_ssl_handshake_start(ngx_event_t *ev);
static void ngx_http_hc_ssl_handshake_done(ngx_connection_t *c);
#endif


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
                "healthcheck: \"ssl\" richiede nginx compilato con "
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
            "healthcheck: \"%V\" richiede nginx compilato con "
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

        hmcf->shm_zone = ngx_shared_memory_add(cf, &name, NGX_HC_SHM_SIZE,
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
                               "\"status\" richiede un solo parametro");
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
                           "status non valido \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (ngx_strcmp(value[0].data, "body") == 0) {

        if (cf->args->nelts != 3 || ngx_strcmp(value[1].data, "~") != 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "sintassi: body ~ \"regex\";");
            return NGX_CONF_ERROR;
        }

        ngx_memzero(&rc, sizeof(ngx_regex_compile_t));

        rc.pattern = value[2];
        rc.pool    = cf->pool;
        rc.err.len = NGX_MAX_CONF_ERRSTR;
        rc.err.data = errstr;

        if (ngx_regex_compile(&rc) != NGX_OK) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "regex \"%V\" non valida: %V",
                               &value[2], &rc.err);
            return NGX_CONF_ERROR;
        }

        match->body_regex   = rc.regex;
        match->body_pattern = value[2];

        return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "direttiva sconosciuta \"%V\" in healthcheck_match",
                       &value[0]);
    return NGX_CONF_ERROR;
}


/* --------------------------------------------------- upstream init hook */

static ngx_int_t
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
                "healthcheck: match \"%V\" non definito (usa "
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
                    "healthcheck: ssl_verify=on richiede "
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
    }

    return NGX_OK;
}


static ngx_int_t
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


/* ------------------------------------------------- balancer interception */

static ngx_int_t
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
    ctx->log           = r->connection->log;

    r->upstream->peer.data = ctx;
    r->upstream->peer.get  = ngx_http_hc_get_peer;
    r->upstream->peer.free = ngx_http_hc_free_peer;

    return NGX_OK;
}


static ngx_int_t
ngx_http_hc_peer_is_down(ngx_http_hc_main_conf_t *hmcf,
    struct sockaddr *sockaddr, socklen_t socklen)
{
    ngx_uint_t           i;
    ngx_http_hc_peer_t  *peers;

    if (hmcf == NULL || hmcf->shm_peers == NULL) {
        return 0;
    }

    peers = hmcf->peers.elts;

    for (i = 0; i < hmcf->peers.nelts; i++) {
        if (ngx_cmp_sockaddr(peers[i].sockaddr, peers[i].socklen,
                             sockaddr, socklen, 1)
            == NGX_OK)
        {
            return hmcf->shm_peers[peers[i].index].down ? 1 : 0;
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

        if (!ngx_http_hc_peer_is_down(ctx->hmcf, pc->sockaddr, pc->socklen)) {
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


/* -------------------------------------------------------- active checks */

static ngx_int_t
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

    peer->busy     = 1;
    peer->recv_len = 0;

    if (peer->request) {
        /* reuse the buffer built on the first probe */
        peer->request->pos = peer->request->start;
    }

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
                "healthcheck: certificato TLS non valido per %V (%V)",
                &peer->name, &peer->upstream);
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

        p = ngx_snprintf(buf, sizeof(buf),
                "GET %V HTTP/1.0" CRLF
                "Host: %V" CRLF
                "User-Agent: nginx-healthcheck" CRLF
                "Connection: close" CRLF CRLF,
                &peer->conf->uri, &peer->upstream);

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
            break;
        }

        peer->recv_len += n;

        if (peer->recv_len >= NGX_HC_RECV_SIZE - 1) {
            break;
        }
    }

    /*
     * connection closed by the peer (or buffer full): evaluate what we
     * have against the configured (or default) match rules
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


static void
ngx_http_hc_finish_check(ngx_http_hc_peer_t *peer, ngx_int_t success)
{
    ngx_http_hc_main_conf_t  *hmcf;
    ngx_http_hc_shm_peer_t   *shm;
    ngx_connection_t         *c;
    ngx_pool_t               *pool;

    if (peer->pc.connection) {
        c = peer->pc.connection;

        if (c->read->timer_set) {
            ngx_del_timer(c->read);
        }
        if (c->write->timer_set) {
            ngx_del_timer(c->write);
        }

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

    peer->busy = 0;

    hmcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
               ngx_http_upstream_healthcheck_module);

    if (hmcf == NULL || hmcf->shm_peers == NULL) {
        return;
    }

    shm = &hmcf->shm_peers[peer->index];

    shm->total_checks++;

    if (success) {
        shm->fall_count = 0;

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
}


/* ----------------------------------------------------------- status page */

static ngx_int_t
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
            shm = &hmcf->shm_peers[peers[i].index];

            b->last = ngx_sprintf(b->last,
                          "%V  peer=%V  status=%s  checks=%ui  fails=%ui\n",
                          &peers[i].upstream, &peers[i].name,
                          shm->down ? "DOWN" : "up",
                          shm->total_checks, shm->total_fails);
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
