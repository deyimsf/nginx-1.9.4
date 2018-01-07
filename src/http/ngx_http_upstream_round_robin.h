
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_UPSTREAM_ROUND_ROBIN_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_ROUND_ROBIN_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct ngx_http_upstream_rr_peer_s   ngx_http_upstream_rr_peer_t;

/**
 * 一个peer,代表一个上游地址
 */
struct ngx_http_upstream_rr_peer_s {
    struct sockaddr                *sockaddr;
    socklen_t                       socklen;
    ngx_str_t                       name;
    ngx_str_t                       server;

    ngx_int_t                       current_weight;
    ngx_int_t                       effective_weight;
    ngx_int_t                       weight;

    ngx_uint_t                      conns;

    ngx_uint_t                      fails;
    time_t                          accessed;
    time_t                          checked;

    ngx_uint_t                      max_fails;
    time_t                          fail_timeout;

    ngx_uint_t                      down;          /* unsigned  down:1; */

#if (NGX_HTTP_SSL)
    void                           *ssl_session;
    int                             ssl_session_len;
#endif

    ngx_http_upstream_rr_peer_t    *next;

#if (NGX_HTTP_UPSTREAM_ZONE)
    ngx_atomic_t                    lock;
#endif
};


typedef struct ngx_http_upstream_rr_peers_s  ngx_http_upstream_rr_peers_t;

/*
 * upstream中的server采用轮训负载均衡时用到的结构体(高效容器),假设有如下upstream配置块
 * 		upstream tomcat {
 * 			server 127.0.0.1:8080;
 * 			server 127.0.0.1:9090;
 * 		}
 * 那么这个结构体就代表上面的配置
 *
 */
struct ngx_http_upstream_rr_peers_s {
	/*
	 * 在ngx_http_upstream_round_robin.c/ngx_http_upstream_init_round_robin()方法中设置
	 *
	 * 代表ip地址个数
	 */
    ngx_uint_t                      number;

#if (NGX_HTTP_UPSTREAM_ZONE)
    ngx_slab_pool_t                *shpool;
    ngx_atomic_t                    rwlock;
    ngx_http_upstream_rr_peers_t   *zone_next;
#endif

    /*
     * 在ngx_http_upstream_round_robin.c/ngx_http_upstream_init_round_robin()方法中设置
     *
     * upstream块下的所有server的权重值总和
     *   n += server[i].naddrs;
     *   w += server[i].naddrs * server[i].weight;
     *   peers->total_weight = w;
     * 其中w就是该字段的值
     */
    ngx_uint_t                      total_weight;

    /*
     * upstream中只有一个server,并且这个server只对应一个ip地址
     */
    unsigned                        single:1;
    unsigned                        weighted:1;

    /*
     * upstream块的名字,比如tomcat
     */
    ngx_str_t                      *name;

    /*
     * upsteam中的另一组peers(比如所有的backup)
     * 目前ngx都是用这个字段来指定backup
     */
    ngx_http_upstream_rr_peers_t   *next;

    /*
     * 当前高效容器中第一个peer(一个IP地址),下一个是peer.next
     */
    ngx_http_upstream_rr_peer_t    *peer;
};


#if (NGX_HTTP_UPSTREAM_ZONE)

#define ngx_http_upstream_rr_peers_rlock(peers)                               \
                                                                              \
    if (peers->shpool) {                                                      \
        ngx_rwlock_rlock(&peers->rwlock);                                     \
    }

#define ngx_http_upstream_rr_peers_wlock(peers)                               \
                                                                              \
    if (peers->shpool) {                                                      \
        ngx_rwlock_wlock(&peers->rwlock);                                     \
    }

#define ngx_http_upstream_rr_peers_unlock(peers)                              \
                                                                              \
    if (peers->shpool) {                                                      \
        ngx_rwlock_unlock(&peers->rwlock);                                    \
    }


#define ngx_http_upstream_rr_peer_lock(peers, peer)                           \
                                                                              \
    if (peers->shpool) {                                                      \
        ngx_rwlock_wlock(&peer->lock);                                        \
    }

#define ngx_http_upstream_rr_peer_unlock(peers, peer)                         \
                                                                              \
    if (peers->shpool) {                                                      \
        ngx_rwlock_unlock(&peer->lock);                                       \
    }

#else

#define ngx_http_upstream_rr_peers_rlock(peers)
#define ngx_http_upstream_rr_peers_wlock(peers)
#define ngx_http_upstream_rr_peers_unlock(peers)
#define ngx_http_upstream_rr_peer_lock(peers, peer)
#define ngx_http_upstream_rr_peer_unlock(peers, peer)

#endif


/*
 * ngx_http_upstream_init_round_robin_peer()方法中会用到这个结构体,
 * 每次请求时都是执行这个方法
 *
 */
typedef struct {
	/*
	 * upstream中所有ip地址的一个高效结构体
	 */
    ngx_http_upstream_rr_peers_t   *peers;

    // 当前使用的peer(upstream中的server)
    ngx_http_upstream_rr_peer_t    *current;
    // 当前server重试了几次
    uintptr_t                      *tried;
    //
    uintptr_t                       data;
} ngx_http_upstream_rr_peer_data_t;


ngx_int_t ngx_http_upstream_init_round_robin(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
ngx_int_t ngx_http_upstream_init_round_robin_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
ngx_int_t ngx_http_upstream_create_round_robin_peer(ngx_http_request_t *r,
    ngx_http_upstream_resolved_t *ur);
ngx_int_t ngx_http_upstream_get_round_robin_peer(ngx_peer_connection_t *pc,
    void *data);
void ngx_http_upstream_free_round_robin_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);

#if (NGX_HTTP_SSL)
ngx_int_t
    ngx_http_upstream_set_round_robin_peer_session(ngx_peer_connection_t *pc,
    void *data);
void ngx_http_upstream_save_round_robin_peer_session(ngx_peer_connection_t *pc,
    void *data);
#endif


#endif /* _NGX_HTTP_UPSTREAM_ROUND_ROBIN_H_INCLUDED_ */
