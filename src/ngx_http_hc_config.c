
/*
 * ngx_http_hc_config.c — parsing of every directive this module defines:
 * "healthcheck" (and its per-parameter-group helpers), "healthcheck_status",
 * "healthcheck_shm_size", "healthcheck_resolver", and the
 * "healthcheck_match NAME { ... }" block.
 */

#include "ngx_http_upstream_healthcheck_module.h"


static ngx_int_t ngx_http_hc_parse_positive_int(ngx_str_t *arg,
    size_t prefix_len, ngx_int_t *out);
static ngx_int_t ngx_http_hc_parse_threshold_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg);
static ngx_int_t ngx_http_hc_parse_connection_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg);
static ngx_int_t ngx_http_hc_parse_request_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg);
static ngx_int_t ngx_http_hc_parse_ssl_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg);
static ngx_int_t ngx_http_hc_register_shm_zone(ngx_conf_t *cf);
static void ngx_http_hc_hook_upstream(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf);
static char *ngx_http_hc_match_directive(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);


/* ---------------------------------------------------- "healthcheck" cmd */

/*
 * Parses the digits after a "name="-style prefix as a positive integer.
 * Returns NGX_ERROR (caller reports "invalid parameter") on anything else,
 * including zero or negative.
 */
static ngx_int_t
ngx_http_hc_parse_positive_int(ngx_str_t *arg, size_t prefix_len,
    ngx_int_t *out)
{
    ngx_str_t  s;
    ngx_int_t  n;

    s.data = arg->data + prefix_len;
    s.len  = arg->len - prefix_len;

    n = ngx_atoi(s.data, s.len);
    if (n == NGX_ERROR || n <= 0) {
        return NGX_ERROR;
    }

    *out = n;

    return NGX_OK;
}


/*
 * Each ngx_http_hc_parse_*_param() below handles one group of "healthcheck"
 * parameters. Return value: NGX_OK (matched and applied), NGX_DECLINED
 * (not one of this group's parameters, try the next group), or NGX_ERROR
 * (matched but invalid — the error is already logged).
 */

static ngx_int_t
ngx_http_hc_parse_threshold_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg)
{
    ngx_int_t  n;

    if (ngx_strncmp(arg->data, "interval=", 9) == 0) {
        if (ngx_http_hc_parse_positive_int(arg, 9, &n) != NGX_OK) {
            goto invalid;
        }
        hscf->interval = (ngx_msec_t) n;
        return NGX_OK;
    }

    if (ngx_strncmp(arg->data, "timeout=", 8) == 0) {
        if (ngx_http_hc_parse_positive_int(arg, 8, &n) != NGX_OK) {
            goto invalid;
        }
        hscf->timeout = (ngx_msec_t) n;
        return NGX_OK;
    }

    if (ngx_strncmp(arg->data, "fall=", 5) == 0) {
        if (ngx_http_hc_parse_positive_int(arg, 5, &n) != NGX_OK) {
            goto invalid;
        }
        hscf->fall = (ngx_uint_t) n;
        return NGX_OK;
    }

    if (ngx_strncmp(arg->data, "rise=", 5) == 0) {
        if (ngx_http_hc_parse_positive_int(arg, 5, &n) != NGX_OK) {
            goto invalid;
        }
        hscf->rise = (ngx_uint_t) n;
        return NGX_OK;
    }

    return NGX_DECLINED;

invalid:
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "healthcheck: invalid parameter \"%V\"", arg);
    return NGX_ERROR;
}


static ngx_int_t
ngx_http_hc_parse_connection_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg)
{
    ngx_int_t  n;

    if (ngx_strcmp(arg->data, "keepalive") == 0) {
        hscf->keepalive = 1;
        return NGX_OK;
    }

    if (ngx_strcmp(arg->data, "resolve") == 0) {
        hscf->resolve = 1;
        return NGX_OK;
    }

    if (ngx_strncmp(arg->data, "resolve_interval=", 17) == 0) {
        if (ngx_http_hc_parse_positive_int(arg, 17, &n) != NGX_OK) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "healthcheck: invalid parameter \"%V\"", arg);
            return NGX_ERROR;
        }
        hscf->resolve_interval = (ngx_msec_t) n;
        return NGX_OK;
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_hc_parse_request_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg)
{
    if (ngx_strncmp(arg->data, "uri=", 4) == 0) {
        hscf->uri.data = arg->data + 4;
        hscf->uri.len  = arg->len - 4;
        if (hscf->uri.len == 0 || hscf->uri.data[0] != '/') {
            goto invalid;
        }
        return NGX_OK;
    }

    if (ngx_strncmp(arg->data, "match=", 6) == 0) {
        hscf->match_name.data = arg->data + 6;
        hscf->match_name.len  = arg->len - 6;
        if (hscf->match_name.len == 0) {
            goto invalid;
        }
        return NGX_OK;
    }

    return NGX_DECLINED;

invalid:
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "healthcheck: invalid parameter \"%V\"", arg);
    return NGX_ERROR;
}


static ngx_int_t
ngx_http_hc_parse_ssl_param(ngx_conf_t *cf,
    ngx_http_hc_srv_conf_t *hscf, ngx_str_t *arg)
{
#if (NGX_SSL)
    ngx_str_t  s;
#endif

    if (ngx_strcmp(arg->data, "ssl") == 0) {
#if (NGX_SSL)
        hscf->ssl = 1;
        return NGX_OK;
#else
        goto ssl_not_compiled;
#endif
    }

    if (ngx_strncmp(arg->data, "ssl_verify=", 11) == 0) {
#if (NGX_SSL)
        s.data = arg->data + 11;
        s.len  = arg->len - 11;

        if (s.len == 2 && ngx_strncmp(s.data, "on", 2) == 0) {
            hscf->ssl_verify = 1;

        } else if (s.len == 3 && ngx_strncmp(s.data, "off", 3) == 0) {
            hscf->ssl_verify = 0;

        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "healthcheck: invalid parameter \"%V\"", arg);
            return NGX_ERROR;
        }
        return NGX_OK;
#else
        goto ssl_not_compiled;
#endif
    }

    if (ngx_strncmp(arg->data, "ssl_name=", 9) == 0) {
#if (NGX_SSL)
        hscf->ssl_name.data = arg->data + 9;
        hscf->ssl_name.len  = arg->len - 9;
        return NGX_OK;
#else
        goto ssl_not_compiled;
#endif
    }

    if (ngx_strncmp(arg->data, "ssl_trusted_certificate=", 24) == 0) {
#if (NGX_SSL)
        hscf->ssl_trusted_certificate.data = arg->data + 24;
        hscf->ssl_trusted_certificate.len  = arg->len - 24;
        return NGX_OK;
#else
        goto ssl_not_compiled;
#endif
    }

    return NGX_DECLINED;

#if !(NGX_SSL)
ssl_not_compiled:
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
        "healthcheck: \"%V\" requires nginx built with "
        "--with-http_ssl_module", arg);
    return NGX_ERROR;
#endif
}


/* registers the shared zone the first time any "healthcheck" is parsed */
static ngx_int_t
ngx_http_hc_register_shm_zone(ngx_conf_t *cf)
{
    ngx_http_hc_main_conf_t  *hmcf;
    ngx_str_t                 name;

    hmcf = ngx_http_conf_get_module_main_conf(cf,
               ngx_http_upstream_healthcheck_module);

    if (hmcf->shm_zone != NULL) {
        return NGX_OK;
    }

    ngx_str_set(&name, NGX_HC_SHM_NAME);

    hmcf->shm_zone = ngx_shared_memory_add(cf, &name,
                         hmcf->shm_size ? hmcf->shm_size : NGX_HC_SHM_SIZE,
                         &ngx_http_upstream_healthcheck_module);
    if (hmcf->shm_zone == NULL) {
        return NGX_ERROR;
    }

    hmcf->shm_zone->init = ngx_http_hc_init_shm_zone;
    hmcf->shm_zone->data = hmcf;

    return NGX_OK;
}


/* wraps this upstream's peer.init_upstream, saving the original */
static void
ngx_http_hc_hook_upstream(ngx_conf_t *cf, ngx_http_hc_srv_conf_t *hscf)
{
    ngx_http_upstream_srv_conf_t  *uscf;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    hscf->original_init_upstream = uscf->peer.init_upstream
                                   ? uscf->peer.init_upstream
                                   : ngx_http_upstream_init_round_robin;

    uscf->peer.init_upstream = ngx_http_hc_init_upstream;
}


char *
ngx_http_hc_healthcheck(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_hc_srv_conf_t  *hscf = conf;
    ngx_str_t               *value;
    ngx_uint_t               i;
    ngx_int_t                rc;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        rc = ngx_http_hc_parse_threshold_param(cf, hscf, &value[i]);
        if (rc == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }
        if (rc == NGX_OK) {
            continue;
        }

        rc = ngx_http_hc_parse_connection_param(cf, hscf, &value[i]);
        if (rc == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }
        if (rc == NGX_OK) {
            continue;
        }

        rc = ngx_http_hc_parse_request_param(cf, hscf, &value[i]);
        if (rc == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }
        if (rc == NGX_OK) {
            continue;
        }

        rc = ngx_http_hc_parse_ssl_param(cf, hscf, &value[i]);
        if (rc == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }
        if (rc == NGX_OK) {
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "healthcheck: invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    hscf->enabled = 1;

    if (ngx_http_hc_register_shm_zone(cf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    ngx_http_hc_hook_upstream(cf, hscf);

    return NGX_CONF_OK;
}


char *
ngx_http_hc_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_hc_status_handler;

    return NGX_CONF_OK;
}


/* --------------------------------------------- "healthcheck_shm_size" */

char *
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

char *
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

char *
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
