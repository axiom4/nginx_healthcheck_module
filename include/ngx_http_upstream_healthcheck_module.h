
/*
 * ngx_http_upstream_healthcheck_module — shared types and cross-file
 * prototypes. See ngx_http_upstream_healthcheck_module.c for the module
 * overview, usage example and design notes.
 */

#ifndef _NGX_HTTP_UPSTREAM_HEALTHCHECK_MODULE_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_HEALTHCHECK_MODULE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_rwlock.h>


#define NGX_HC_SHM_NAME  "upstream_healthcheck"
#define NGX_HC_SHM_SIZE  (128 * 1024)
#define NGX_HC_RECV_SIZE 1024


/* ---------------------------------------------------------------- types */

typedef struct {
    ngx_atomic_t                        lock;       /* protects this struct */
    ngx_uint_t                          fall_count;
    ngx_uint_t                          rise_count;
    ngx_uint_t                          down;
    ngx_uint_t                          total_fails; /* since the last
                                                       * success; reset on
                                                       * each successful
                                                       * probe (see
                                                       * last_fails)       */
    ngx_uint_t                          last_fails; /* value total_fails
                                                       * had right before
                                                       * the most recent
                                                       * reset            */
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
    ngx_flag_t                          keepalive;
    ngx_flag_t                          resolve;
    ngx_msec_t                          resolve_interval;

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
    ngx_http_upstream_rr_peers_t       *rr_peers;   /* for the peer lock   */
    ngx_http_upstream_rr_peer_t        *rr_peer;

    ngx_str_t                           hostname;   /* empty = literal ip,
                                                       * nothing to resolve */
    in_port_t                           port;
    ngx_event_t                         resolve_ev; /* periodic re-resolve */
    ngx_resolver_ctx_t                 *resolve_ctx;

    /*
     * a private copy of cycle->log with .handler/.data set to this peer,
     * so every message logged through the probe connection (including
     * low-level ones like "recv() failed" that know nothing about us)
     * gets this peer's identity appended — see ngx_http_hc_log_error().
     * Must NOT just point at cycle->log directly: that object is shared
     * by the whole process, and every peer would stomp on each other's
     * handler/data.
     */
    ngx_log_t                           log;

    ngx_event_t                         check_ev;   /* periodic timer      */
    ngx_peer_connection_t               pc;
    ngx_buf_t                          *request;
    u_char                              recv[NGX_HC_RECV_SIZE];
    ngx_uint_t                          recv_len;
    ngx_int_t                           header_end;   /* -1 = not found yet */
    ngx_int_t                           content_length; /* -1 = unknown     */
    unsigned                            busy:1;
    unsigned                            conn_open:1;  /* idle, kept alive
                                                         * for the next probe */
    unsigned                            keepalive_ok:1; /* this cycle's
                                                         * response was
                                                         * cleanly framed and
                                                         * may be reused    */
#if (NGX_SSL)
    ngx_ssl_session_t                  *ssl_session; /* cached for reuse   */
#endif
} ngx_http_hc_peer_t;


typedef struct {
    ngx_array_t                         peers;      /* ngx_http_hc_peer_t  */
    ngx_array_t                         matches;    /* ngx_http_hc_match_t */
    ngx_shm_zone_t                     *shm_zone;
    ngx_http_hc_shm_peer_t             *shm_peers;  /* set at zone init    */
    size_t                               shm_size;   /* 0 = default        */
    ngx_resolver_t                     *resolver;   /* for "resolve"       */
} ngx_http_hc_main_conf_t;


/* per-request wrapper around the original balancer */
typedef struct {
    void                               *data;
    ngx_event_get_peer_pt               original_get;
    ngx_event_free_peer_pt              original_free;
#if (NGX_SSL)
    ngx_event_set_peer_session_pt       original_set_session;
    ngx_event_save_peer_session_pt      original_save_session;
#endif
    ngx_http_hc_main_conf_t            *hmcf;
    ngx_http_hc_srv_conf_t             *hscf;
    ngx_log_t                          *log;
} ngx_http_hc_balancer_ctx_t;


/* ------------------------------------------------------- module symbol */

extern ngx_module_t ngx_http_upstream_healthcheck_module;


/* ------------------------------------------------ cross-file prototypes */

/* ngx_http_hc_config.c */
char *ngx_http_hc_healthcheck(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char *ngx_http_hc_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char *ngx_http_hc_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char *ngx_http_hc_resolver(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char *ngx_http_hc_match_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/* ngx_http_hc_upstream.c */
ngx_int_t ngx_http_hc_init_upstream(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
ngx_int_t ngx_http_hc_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data);

/* ngx_http_hc_balancer.c */
ngx_int_t ngx_http_hc_init_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);

/* ngx_http_hc_probe.c */
ngx_int_t ngx_http_hc_init_process(ngx_cycle_t *cycle);

/* ngx_http_hc_resolve.c */
void ngx_http_hc_resolve_begin(ngx_event_t *ev);

/* ngx_http_hc_status.c */
ngx_int_t ngx_http_hc_status_handler(ngx_http_request_t *r);


#endif /* _NGX_HTTP_UPSTREAM_HEALTHCHECK_MODULE_H_INCLUDED_ */
