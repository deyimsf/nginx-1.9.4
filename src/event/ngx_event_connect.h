
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_EVENT_CONNECT_H_INCLUDED_
#define _NGX_EVENT_CONNECT_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define NGX_PEER_KEEPALIVE           1
#define NGX_PEER_NEXT                2
#define NGX_PEER_FAILED              4


typedef struct ngx_peer_connection_s  ngx_peer_connection_t;

typedef ngx_int_t (*ngx_event_get_peer_pt)(ngx_peer_connection_t *pc,
    void *data);
typedef void (*ngx_event_free_peer_pt)(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state);
#if (NGX_SSL)

typedef ngx_int_t (*ngx_event_set_peer_session_pt)(ngx_peer_connection_t *pc,
    void *data);
typedef void (*ngx_event_save_peer_session_pt)(ngx_peer_connection_t *pc,
    void *data);
#endif


/* ngx主动发起的连接 */
struct ngx_peer_connection_s {
    ngx_connection_t                *connection;

    /*
     * Address of the upstream server to connect to; this is the output parameter of a load-balancing method.
     *
     * 下面这三个参数用来描述一个上游服务器的sockaddr地址和名字,比如:
     * 		127.0.0.1:8080
     */
    struct sockaddr                 *sockaddr;
    socklen_t                        socklen;
    ngx_str_t                       *name;

    /*
     * Allowed number of attempts to connect to an upstream server
     *
     * 允许尝试连接上游服务器的次数,比如proxy_next_upstream_tries指令
     */
    ngx_uint_t                       tries;
    ngx_msec_t                       start_time;

    /*
     * The method called when the upstream module is ready to pass a request to an upstream
     * server and needs to know its address. The method has to fill the sockaddr, socklen, and
     * name fields of ngx_peer_connection_t structure.
     *
     * 当一个请求准备向上游发起请求时,需要获取一个上游的地址,这个方法就是用来根据负载算法选择一个上游地址的,选择好后
     * 会把代表上游地址的sockaddr、socklen、name信息填充到该结构体对应的字段中
     *
     * 根据负载均衡规则获取一个上游服务器信息
     *
     * 该方法接收两个参数:
     *	 1.一个ngx_peer_connection_t对象
     *	 2.一个在ngx_http_upstream_srv_conf_t.peer.init()方法中创建的data数据
     *
     * 这个方法可以返回如下值:
     *   NGX_OK — Server was selected.
     *   NGX_ERROR — Internal error occurred.
     *   NGX_BUSY — no servers are currently available. This can happen due to many reasons, including:
     *   			the dynamic server group is empty, all servers in the group are in the failed state, or
     *   			all servers in the group are already handling the maximum number of connections.
     *   NGX_DONE — the underlying connection was reused and there is no need to create a new connection
     *   			to the upstream server. This value is set by the keepalive module.
     */
    ngx_event_get_peer_pt            get;

    /*
     * free(pc, data, state)
     * The method called when an upstream module has finished work with a particular server.
     * The state argument is the completion status of the upstream connection, a bitmask with the following possible values:
     * 	  NGX_PEER_FAILED — Attempt was unsuccessful
     *    NGX_PEER_NEXT — A special case when upstream server returns codes 403 or 404, which are not considered a failure.
     *    NGX_PEER_KEEPALIVE — Currently unused
     * This method also decrements the tries counter.
     *
     * 当结束一个上游服务调用后调用该方法
     *
     * 这个方法需要对tries字段做递减操作
     */
    ngx_event_free_peer_pt           free;

    /*
     * TODO 不是太明白
     *
     * The per-request data of a load-balancing method;
     * keeps the state of the selection algorithm and usually includes the link to the upstream configuration.
     * It is passed as an argument to all methods that deal with server selection (see below).
     *
     * 如果是默认轮询负载就是 ngx_http_upstream_rr_peer_data_t
     * 在ngx_http_upstream_round_robin.c/ngx_http_upstream_init_round_robin_peer()方法中有设置
     */
    void                            *data;

#if (NGX_SSL)
    ngx_event_set_peer_session_pt    set_session;
    ngx_event_save_peer_session_pt   save_session;
#endif

    ngx_addr_t                      *local;

    int                              rcvbuf;

    ngx_log_t                       *log;

    unsigned                         cached:1;

                                     /* ngx_connection_log_error_e */
    unsigned                         log_error:2;
};


ngx_int_t ngx_event_connect_peer(ngx_peer_connection_t *pc);
ngx_int_t ngx_event_get_peer(ngx_peer_connection_t *pc, void *data);


#endif /* _NGX_EVENT_CONNECT_H_INCLUDED_ */
