
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_UPSTREAM_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_connect.h>
#include <ngx_event_pipe.h>
#include <ngx_http.h>


#define NGX_HTTP_UPSTREAM_FT_ERROR           0x00000002
#define NGX_HTTP_UPSTREAM_FT_TIMEOUT         0x00000004
#define NGX_HTTP_UPSTREAM_FT_INVALID_HEADER  0x00000008
#define NGX_HTTP_UPSTREAM_FT_HTTP_500        0x00000010
#define NGX_HTTP_UPSTREAM_FT_HTTP_502        0x00000020
#define NGX_HTTP_UPSTREAM_FT_HTTP_503        0x00000040
#define NGX_HTTP_UPSTREAM_FT_HTTP_504        0x00000080
#define NGX_HTTP_UPSTREAM_FT_HTTP_403        0x00000100
#define NGX_HTTP_UPSTREAM_FT_HTTP_404        0x00000200
#define NGX_HTTP_UPSTREAM_FT_UPDATING        0x00000400
#define NGX_HTTP_UPSTREAM_FT_BUSY_LOCK       0x00000800
#define NGX_HTTP_UPSTREAM_FT_MAX_WAITING     0x00001000
#define NGX_HTTP_UPSTREAM_FT_NOLIVE          0x40000000
#define NGX_HTTP_UPSTREAM_FT_OFF             0x80000000

#define NGX_HTTP_UPSTREAM_FT_STATUS          (NGX_HTTP_UPSTREAM_FT_HTTP_500  \
                                             |NGX_HTTP_UPSTREAM_FT_HTTP_502  \
                                             |NGX_HTTP_UPSTREAM_FT_HTTP_503  \
                                             |NGX_HTTP_UPSTREAM_FT_HTTP_504  \
                                             |NGX_HTTP_UPSTREAM_FT_HTTP_403  \
                                             |NGX_HTTP_UPSTREAM_FT_HTTP_404)

#define NGX_HTTP_UPSTREAM_INVALID_HEADER     40


#define NGX_HTTP_UPSTREAM_IGN_XA_REDIRECT    0x00000002
#define NGX_HTTP_UPSTREAM_IGN_XA_EXPIRES     0x00000004
#define NGX_HTTP_UPSTREAM_IGN_EXPIRES        0x00000008
#define NGX_HTTP_UPSTREAM_IGN_CACHE_CONTROL  0x00000010
#define NGX_HTTP_UPSTREAM_IGN_SET_COOKIE     0x00000020
#define NGX_HTTP_UPSTREAM_IGN_XA_LIMIT_RATE  0x00000040
#define NGX_HTTP_UPSTREAM_IGN_XA_BUFFERING   0x00000080
#define NGX_HTTP_UPSTREAM_IGN_XA_CHARSET     0x00000100
#define NGX_HTTP_UPSTREAM_IGN_VARY           0x00000200


typedef struct {
    ngx_msec_t                       bl_time;
    ngx_uint_t                       bl_state;

    ngx_uint_t                       status;
    /* 记录上游服务器响应的开始时间 */
    ngx_msec_t                       response_time;
    ngx_msec_t                       connect_time;
    /* 记录解析上游服务器响应头用掉的事件 */
    ngx_msec_t                       header_time;
    off_t                            response_length;

    /*
     * 对应的上游服务器的字符地址,比如:
     *   upsteam tomcat {
     *   	server 127.0.0.1:8080;
     *   	server 127.0.0.1:80;
     *   }
     * 是上面这个配置中的某个地址
     */
    ngx_str_t                       *peer;
} ngx_http_upstream_state_t;


typedef struct {
	/*
	 * 用于存放ngx_http_upstream.c/ngx_http_upstream_headers_in[]数组的hash结构
	 */
    ngx_hash_t                       headers_in_hash;

    /*
     * 数组中存放着ngx_http_upstream_srv_conf_t对象
     */
    ngx_array_t                      upstreams;

} ngx_http_upstream_main_conf_t;

/*
 * 对应一个upstream{}配置块
 */
typedef struct ngx_http_upstream_srv_conf_s  ngx_http_upstream_srv_conf_t;

typedef ngx_int_t (*ngx_http_upstream_init_pt)(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
typedef ngx_int_t (*ngx_http_upstream_init_peer_pt)(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);


/*
 * A module that implements a load-balancing algorithm must set these methods and initialize private data.
 * If init_upstream was not initialized during configuration parsing, ngx_http_upstream_module sets it to
 * the default ngx_http_upstream_init_round_robin algorithm.
 *
 * 如果第三方模块需要实现一个负载均衡算法,那么他必须设置这个结构体中的方法和data字段
 *
 */
typedef struct {
	/*
	 * Configuration-time method responsible for initializing a group of servers and initializing the init()
	 * method in case of success. A typical load-balancing module uses a list of servers in the upstream block
	 * to create an efficient data structure that it uses and saves its own configuration to the data field.
	 *
	 * 第三方模块可以通过设置这个字段来定义自己的负载均衡策略,目前ngx官方用的这个的模块有:
	 *		ngx_http_upstream_ip_hash_module.c/ngx_http_upstream_ip_hash()方法,该方法对应ip_hash指令
	 *		ngx_http_upstream_hash_module.c/ngx_http_upstream_hash()方法,该方法对应upstream中的hash指令
	 *		ngx_http_upstream_keepalive_module.c/ngx_http_upstream_keepalive()方法,对应keepalive指令
	 *		ngx_http_upstream_least_conn_module.c/ngx_http_upstream_least_conn()方法,对应least_conn指令
	 *
	 * 第三方模块需要在这个方法中把ngx_http_upstream_srv_conf_t->servers中的server组装成一个特定容器(比如ngx_http_upstream_rr_peers_t)
	 * 然后把这个容器(高效的数据结构)赋值给该结构体的data字段
	 *
	 * 第三方模块需要在这个方法中设置该结构体的init字段
	 *
	 * 这个方法在ngx配置文件解析过程中执行(ngx_http_upstream_init_main_conf()方法)
	 */
    ngx_http_upstream_init_pt        init_upstream;

    /*
     * Initializes a per-request ngx_http_upstream_peer_t.peer structure that is used for load
     * balancing (not to be confused with the ngx_http_upstream_srv_conf_t.peer described above which
     * is per-upstream). It is passed as the data argument to all callbacks that deal with server selection.
     *
     * 这个方法在每次请求时执行
     *
     * 在这个方法中需要设置ngx_http_upstream_peer_t.peer结构体中的回调方法,这些回调方法用来实现真正的负债均衡,比如:
     * 	     r->ngx_http_upstream_t->peer.get = ngx_http_upstream_get_round_robin_peer;
     *       r->ngx_http_upstream_t->peer.free = ngx_http_upstream_free_round_robin_peer;
     *       r->ngx_http_upstream_t->peer.tries = ngx_http_upstream_tries(rrp->peers);
     * 其中peer是结构体ngx_peer_connection_s
     *
     */
    ngx_http_upstream_init_peer_pt   init;

    /*
     * 由init_upstream方法构造的特定容器(用来存储server的高效结构体,比如ngx_http_upstream_rr_peers_t)
     */
    void                            *data;
} ngx_http_upstream_peer_t;

/*
 * 对应upstream中的一个server指令信息
 */
typedef struct {
	/*
	 * 可以是upstream中server指令指定的,比如
	 * 		server 127.0.0.1:8080
	 * 		server www.jd.com
	 *
	 */
    ngx_str_t                        name;
    // 套接字地址,包括端口
    ngx_addr_t                      *addrs;
    // ?
    ngx_uint_t                       naddrs;
    // 该server的权重
    ngx_uint_t                       weight;
    // 最大失败次数
    ngx_uint_t                       max_fails;
    // 失败后多长时间不可用
    time_t                           fail_timeout;

    /*
     * marks the server as permanently unavailable
     * server是否可用
     */
    unsigned                         down:1;
    // 是否是备份server
    unsigned                         backup:1;
} ngx_http_upstream_server_t;


/*
 * 代表u中的信息是一个标准的upsteam配置,不是一个url(比如prox_pass可以直接写url)
 */
#define NGX_HTTP_UPSTREAM_CREATE        0x0001
#define NGX_HTTP_UPSTREAM_WEIGHT        0x0002
#define NGX_HTTP_UPSTREAM_MAX_FAILS     0x0004
#define NGX_HTTP_UPSTREAM_FAIL_TIMEOUT  0x0008
#define NGX_HTTP_UPSTREAM_DOWN          0x0010
#define NGX_HTTP_UPSTREAM_BACKUP        0x0020


/**
 * 对应一个upstream配置块,比如
 * 	 upstream tomcat {
 * 		 server ...;
 * 	 }
 *
 */
struct ngx_http_upstream_srv_conf_s {
	/*
	 * object that holds generic methods for initializing upstream configuration
	 *
	 * 初始化upstram配置的一个对象,如果第三方模块要实现一个负载算法就需要设置该字段中的一些值
	 *
	 * 比如peer.data:
	 * 	  由init_upstream方法构造的特定容器(用来存储server的高效结构体,比如ngx_http_upstream_rr_peers_t)
	 */
    ngx_http_upstream_peer_t         peer;

    /*
     * Configuration context of upstream modules.
     *
     * ngx_http_conf_ctx_t->srv_conf
     *
     * 用来存储所有http模块在srv_conf区域的配置信息结构体,当然这里包括ngx_http_upstream_srv_conf_t结构体
     * 但是目前在ngx代码中没有看到使用这个字段的地方(除了在ngx_http_upstream()方法中对他的一个赋值)
     *
     * 		   srv_conf
     * 			-----
     * 			| * |
     * 			-----
     * 			  \
     * 		---------------
     * 		| * | ... | * | ngx_http_max_module个
     * 		---------------
     * 				\ngx_http_upstream_module.ctx_index
     * 		   ------------------------------
     * 		   |ngx_http_upstream_srv_conf_t|
     * 		   ------------------------------
     */
    void                           **srv_conf;

    /*
     * Array of ngx_http_upstream_server_t, the result of parsing a set of server directives in the upstream block.
     *
     * 该upstream下所有的server
     */
    ngx_array_t                     *servers;  /* ngx_http_upstream_server_t */

    /*
     * NGX_HTTP_UPSTREAM_CREATE:
     * 		Distinguishes explicitly defined upstreams from those that are automatically created
     * 		by the proxy_pass directive and “friends” (FastCGI, SCGI, etc.)
     *
     * 		用来区分upstream配置信息是用upstream指令定义的还是其它类似proxy_pass指令定义的
     *
     * NGX_HTTP_UPSTREAM_WEIGHT:
     * 		The “weight” parameter is supported
     *
     * NGX_HTTP_UPSTREAM_MAX_FAILS:
     * 		The “max_fails” parameter is supported
     *
     * NGX_HTTP_UPSTREAM_FAIL_TIMEOUT:
     * 		The “fail_timeout” parameter is supported
     *
     * NGX_HTTP_UPSTREAM_DOWN:
     * 		The “down” parameter is supported
     *
     * NGX_HTTP_UPSTREAM_BACKUP:
     * 		The “backup” parameter is supported
     *
     * NGX_HTTP_UPSTREAM_MAX_CONNS:
     * 		The “max_conns” parameter is supported
     */
    ngx_uint_t                       flags;

    /*
     * Name of the upstream
     *
     * upstream的名字,比如上面的tomcat
     */
    ngx_str_t                        host;

    /*
     * Name of the configuration file and the line where the upstream block is located
     *
     * upstream块所在文件的名字和行号
     */
    u_char                          *file_name;
    ngx_uint_t                       line;

    /*
     * upstream配置块没有使用,但是proxy_pass等指令有使用
     */
    in_port_t                        port;
    in_port_t                        default_port;
    ngx_uint_t                       no_port;  /* unsigned no_port:1 */

#if (NGX_HTTP_UPSTREAM_ZONE)
    ngx_shm_zone_t                  *shm_zone;
#endif
};


typedef struct {
    ngx_addr_t                      *addr;
    ngx_http_complex_value_t        *value;
} ngx_http_upstream_local_t;


/*
 * 某个请求对应的配置信息,比如连接上游服务器的超时时间,向上游发送数据的超时时间,从上游读数据时的超时时间等
 */
typedef struct {

	/*
	 * 当前请求用到的upstream{}块信息(或者类似proxy_pass指定的url)
	 */
    ngx_http_upstream_srv_conf_t    *upstream;

    ngx_msec_t                       connect_timeout;
    ngx_msec_t                       send_timeout;
    ngx_msec_t                       read_timeout;
    ngx_msec_t                       timeout;
    ngx_msec_t                       next_upstream_timeout;

    size_t                           send_lowat;
    size_t                           buffer_size;
    size_t                           limit_rate;

    size_t                           busy_buffers_size;
    size_t                           max_temp_file_size;
    size_t                           temp_file_write_size;

    size_t                           busy_buffers_size_conf;
    size_t                           max_temp_file_size_conf;
    size_t                           temp_file_write_size_conf;

    /*
     * upstream在读取上游返回的数据的时候会用到buffer,具体创建几个buffer以及buf有多大则有该字段
     * 决定,这个字段跟copy_filter过滤器中用到的bufs功能类似.
     *
     * 在proxy_pass中这个字段由proxy_buffers指令设置:
     * 	 Sets the number and size of the buffers used for reading a response
     *   from the proxied server, for a single connection.
     *
     * 这个字段只是用来设置接收响应体数据时用到的buffer的个数和大小,用于接收响应头的buffer使用的是
     * ngx_http_upstream_s结构体的buffer字段.
     */
    ngx_bufs_t                       bufs;

    ngx_uint_t                       ignore_headers;
    ngx_uint_t                       next_upstream;
    ngx_uint_t                       store_access;
    ngx_uint_t                       next_upstream_tries;
    ngx_flag_t                       buffering;
    ngx_flag_t                       request_buffering;
    ngx_flag_t                       pass_request_headers;
    ngx_flag_t                       pass_request_body;

    ngx_flag_t                       ignore_client_abort;
    ngx_flag_t                       intercept_errors;
    ngx_flag_t                       cyclic_temp_file;
    ngx_flag_t                       force_ranges;

    ngx_path_t                      *temp_path;

    ngx_hash_t                       hide_headers_hash;
    ngx_array_t                     *hide_headers;
    ngx_array_t                     *pass_headers;

    ngx_http_upstream_local_t       *local;

#if (NGX_HTTP_CACHE)
    ngx_shm_zone_t                  *cache_zone;
    ngx_http_complex_value_t        *cache_value;

    ngx_uint_t                       cache_min_uses;
    ngx_uint_t                       cache_use_stale;
    ngx_uint_t                       cache_methods;

    ngx_flag_t                       cache_lock;
    ngx_msec_t                       cache_lock_timeout;
    ngx_msec_t                       cache_lock_age;

    ngx_flag_t                       cache_revalidate;

    ngx_array_t                     *cache_valid;
    ngx_array_t                     *cache_bypass;
    ngx_array_t                     *no_cache;
#endif

    ngx_array_t                     *store_lengths;
    ngx_array_t                     *store_values;

#if (NGX_HTTP_CACHE)
    signed                           cache:2;
#endif
    signed                           store:2;
    unsigned                         intercept_404:1;
    unsigned                         change_buffering:1;

#if (NGX_HTTP_SSL)
    ngx_ssl_t                       *ssl;
    ngx_flag_t                       ssl_session_reuse;

    ngx_http_complex_value_t        *ssl_name;
    ngx_flag_t                       ssl_server_name;
    ngx_flag_t                       ssl_verify;
#endif

    ngx_str_t                        module;
} ngx_http_upstream_conf_t;


typedef struct {
    ngx_str_t                        name;
    ngx_http_header_handler_pt       handler;
    ngx_uint_t                       offset;
    ngx_http_header_handler_pt       copy_handler;
    ngx_uint_t                       conf;
    ngx_uint_t                       redirect;  /* unsigned   redirect:1; */
} ngx_http_upstream_header_t;


typedef struct {
    ngx_list_t                       headers;

    ngx_uint_t                       status_n;
    ngx_str_t                        status_line;

    ngx_table_elt_t                 *status;
    ngx_table_elt_t                 *date;
    ngx_table_elt_t                 *server;
    ngx_table_elt_t                 *connection;

    ngx_table_elt_t                 *expires;
    ngx_table_elt_t                 *etag;
    ngx_table_elt_t                 *x_accel_expires;
    ngx_table_elt_t                 *x_accel_redirect;
    ngx_table_elt_t                 *x_accel_limit_rate;

    ngx_table_elt_t                 *content_type;
    ngx_table_elt_t                 *content_length;

    ngx_table_elt_t                 *last_modified;
    ngx_table_elt_t                 *location;
    ngx_table_elt_t                 *accept_ranges;
    ngx_table_elt_t                 *www_authenticate;
    ngx_table_elt_t                 *transfer_encoding;
    ngx_table_elt_t                 *vary;

#if (NGX_HTTP_GZIP)
    ngx_table_elt_t                 *content_encoding;
#endif

    ngx_array_t                      cache_control;
    ngx_array_t                      cookies;

    off_t                            content_length_n;
    time_t                           last_modified_time;

    unsigned                         connection_close:1;
    unsigned                         chunked:1;
} ngx_http_upstream_headers_in_t;


typedef struct {
    ngx_str_t                        host;
    in_port_t                        port;
    ngx_uint_t                       no_port; /* unsigned no_port:1 */

    ngx_uint_t                       naddrs;
    ngx_addr_t                      *addrs;

    struct sockaddr                 *sockaddr;
    socklen_t                        socklen;

    ngx_resolver_ctx_t              *ctx;
} ngx_http_upstream_resolved_t;


typedef void (*ngx_http_upstream_handler_pt)(ngx_http_request_t *r,
    ngx_http_upstream_t *u);


/*
 * 里面存放了实现upstream的一些钩子,比如实现upstream的各个回调函数
 * 第三方模块在实现upstream的时候需要为该对象中的回调方法赋值,比如如何创建请求(create_request)
 * 如何处理上游返回的数据(process_header)
 */
struct ngx_http_upstream_s {
    ngx_http_upstream_handler_pt     read_event_handler;
    ngx_http_upstream_handler_pt     write_event_handler;

    /*
     * When nginx has to pass a request to another host for processing, it uses the configured load-balancing
     * method to obtain an address to connect to. The method is obtained from the ngx_http_upstream_t.peer
     * object of type ngx_peer_connection_t
     *
     * 代表一个上游连接.
     *
     * 另一个作用,当ngx的一个请求需要另一个服务器来完成的时候,ngx需要通过配置好的负载均衡策略去选择一个上游服务器来连接,获取这个上游地址
     * 的方法被负载均衡模块放在了peer.get中
     */
    ngx_peer_connection_t            peer;

    ngx_event_pipe_t                *pipe;

    /*
     * 任何三方模块使用upstream时,都会把协议的请求数据放到这个链表中
     * upsteam模块通过回调u->create_request(r)方法来触发三方模块的实现
     *
     * 比如proxy模块对应的u->create_request()回调方法是ngx_http_proxy_create_request()方法,该方法
     * 会把要发送的http协议头组装好并赋值给request_bufs这个链表
     */
    ngx_chain_t                     *request_bufs;

    /*
     * 调用ngx_output_chain(&u->output, out)方法向外发送数据的一个上下文,其中
     * u->output就是这个字段
     *
     * copy_filter模块发送数据是也用到了这个结构体对象
     */
    ngx_output_chain_ctx_t           output;
    /*
     * TODO 什么时候设置的
     */
    ngx_chain_writer_ctx_t           writer;

    /*
     * TODO
     * 某个请求对应的配置信息
     */
    ngx_http_upstream_conf_t        *conf;
#if (NGX_HTTP_CACHE)
    ngx_array_t                     *caches;
#endif

    /*
     * 这个字段用来存放从上游获取的响应头,当然如果上游服务器并不是http协议,比如是redis,那就需要
     * redis模块开发者根据redis的返回情况来设置这个字段中的值了
     *
     * 第三方开发者只需要设置这个头(u->headers_in),并不需要设置他对应的r中的头(r->headers_out)
     *
     * 协议头解析完毕之后调用ngx_http_upstream_process_headers()方法,该方法会把在u->headers_in.headers
     * 中的头信息拷贝到r->headers_out.headers中,具体拷贝哪些响应头以及如何拷贝由ngx_http_upstream_headers_in[]
     * 数组决定
     */
    ngx_http_upstream_headers_in_t   headers_in;

    /* TODO 干啥的 */
    ngx_http_upstream_resolved_t    *resolved;

    ngx_buf_t                        from_client;

    /*
     * 用来接收响应中第一部分数据的缓存buffer,其中buffer的大小由u->conf->buffer_size决定,这个值必须指定
     *
     * 在proxy_pass中由proxy_buffer_size指令指定
     * 在memcached中用memcached_buffer_size指令指定
     * 在fastcgi中用fastcgi_buffer_size指令指定
	 * 在scgi中用scgi_buffer_size指令指定
	 * 在uwsgi中用uwsgi_buffer_size指令指定
	 *
	 * 在真正接收响应体的时候并不是用的这个字段,upstream在接收响应体的时候会用到多个buffer,具体可以创建多少
	 * 个buffer以及这些buffer的大小则有ngx_http_upstream_conf_t结构体的bufs字段决定
     */
    ngx_buf_t                        buffer;
    /* TODO */
    off_t                            length;

    /*
     * 当禁用buffering功能的时候ngx就会不用多个缓存块来接收响应数据,而是用单个缓存块来接收响应数据,一旦接收
     * 到后就会立即输出,在输出之前ngx会把从上游收到的数据追加到out_bufs这个链表尾部.
     *
     * 当开启buffering功能的时候就不会用到该字段了,开启缓存的时候会用到ngx_event_pipe_s结构体中的in和out,
     * 详情可以查看ngx_event_pipe_s结构体中对in和out的注释
     *
     * 这里有一个问题,既然只用一块缓存来接收响应数据,为什么在输出的时候还要用一个链表来暂存数据?
     * 假设存在这样一个场景,客户端和上游服务器网络都不好,每次只能从上游服务器读取整个响应的一部分数据,而且读到的
     * 这些数据也因为客户端网络不好的问题,也没办法每次都百分之百输出到客户端,这种情况用图表示如下:
     *
     *  	    out_bufs
     *		 ----------------     ----------------	   ----------------
     *		 |  *buf  |*next| --> |  *buf  |*next| --> |  *buf  |*next|
     *		 ----------------     ----------------	   ----------------
     *		  /      \			  /	     \			   /        \
     *      -----------------------------------------------------------
     *      |														  |
     *		-----------------------------------------------------------
     *		 \														 /
     *		  \													    /
     *		   -----------------------------------------------------
     *		   | 		   *start        |	       *end            |
     *		   -----------------------------------------------------
     *								   buffer
     * 可以看到链表out_bufs中的多个链表项可以公用同一块缓存,不一样的地方是各自链表项中buf的pos和last的变量值
     */
    ngx_chain_t                     *out_bufs;
    ngx_chain_t                     *busy_bufs;
    ngx_chain_t                     *free_bufs;

    ngx_int_t                      (*input_filter_init)(void *data);
    /*
     * 当u->buffering = 0也就是不使用缓存功能时,ngx在读取到上游部分数据后会回调这个方法.所以必须设置
     * 当u->buffering = 1也就是使用多缓存块时,ngx就不会回调这个方法,所以可以不设置
     *
     * 当接收到上游数据并准备向客户端输出之前会调用这个方法,这个方法的主要作用是把从上游读到的数据追加到
     * u->out_bufs链表中,因为后续向客户端发送的数据都是从u->out_bufs链表中取的.
     *
     * 比如proxy模块和memcached模块都用到了这个方法,分别是:
     * 	  /src/http/modules/ngx_http_proxy_module.c/ngx_http_proxy_non_buffered_copy_filter()
     * 	  /src/http/modules/ngx_http_memcached_module.c/ngx_http_memcached_filter()
     *
     *
     * 这里的input有这样一层含义,把外部数据搬运到内部
     * 比如:(ngx_http_proxy_module.c/ngx_http_proxy_handler方法中)
     * 	   u->input_filter = ngx_http_proxy_non_buffered_copy_filter;
     * ngx_http_proxy_non_buffered_copy_filter()方法把u->buffer中的数据搬运到u->out_bufs中
     *
     * 比如:(ngx_http_proxy_module.c/ngx_http_proxy_handler方法中)
     * 	  u->pipe->input_filter = ngx_http_proxy_copy_filter;
     * ngx_http_proxy_copy_filter()方法把buf中的数据追加到pipe->in中
     */
    ngx_int_t                      (*input_filter)(void *data, ssize_t bytes);
    /*
     * upstream在回调input_filter()方法时,会把这个字段传递给这个方法的第一个参数
     * 大部分时候这个字段值是请求对象r
     */
    void                            *input_filter_ctx;

#if (NGX_HTTP_CACHE)
    ngx_int_t                      (*create_key)(ngx_http_request_t *r);
#endif
    /*
     * 必须设置
     *
     * 用来创建请求的回调方法
     * 第三方模块在实现这个函数时要把向上游发送的协议头内容赋值给u->request_bufs链表
     *
     * 一个请求只调用一次
     *
     */
    ngx_int_t                      (*create_request)(ngx_http_request_t *r);
    /*
     * 必须设置
     *
     * 一个重新初始化请求的回调方法,每次调用ngx_http_upstream_connect()方法时,都会在这个方法中检查是否已经发送过请求,
     * 如果发送过则通过ngx_http_upstream_reinit()方法来回调u->reinit_request(r)[会被多次调用]方法,否做就不回调.
     *
     * 这个方法目前没发现什么潜规则,第三发模块自己决定该做什么事
     *
     * 一个请求可能被多次调用
     *
     */
    ngx_int_t                      (*reinit_request)(ngx_http_request_t *r);
    /*
     * 必须设置
     *
     * 负责处理上游返回的响应头,ngx仅用一块u->buffer来接收响应头数据,如果响应头数据大于这个缓存块可承受的容量,ngx就不玩了.
     * 这个方法的潜规则是,第三方模块要更改u->buffer->pos的值,比如proxy模块中ngx_http_proxy_process_status_line()
     * 方法,它会通过调用ngx_http_parse_status_line()方法来把响应行数据给吃掉(u->buffer->pos=响应行最后一个字符的后面)
     *
     * 一个请求可能被多次调用
     *
     */
    ngx_int_t                      (*process_header)(ngx_http_request_t *r);
    /* 目前没用到 */
    void                           (*abort_request)(ngx_http_request_t *r);
    /*
     * 必须设置
     *
     * 结束upstream请求,在ngx_http_upstream_finalize_request()方法中调用
     * 一个请求只调用一次
     *
     */
    void                           (*finalize_request)(ngx_http_request_t *r,
                                         ngx_int_t rc);
    ngx_int_t                      (*rewrite_redirect)(ngx_http_request_t *r,
                                         ngx_table_elt_t *h, size_t prefix);
    ngx_int_t                      (*rewrite_cookie)(ngx_http_request_t *r,
                                         ngx_table_elt_t *h);

    ngx_msec_t                       timeout;

    /*
     * 当前请求对应的一个确定的上游服务的状态
     * 比如:
     *   upsteam tomcat {
     *   	server 127.0.0.1:8080;
     *   	server 127.0.0.1:80;
     *   }
     * 是上面这个配置中的某个ip
     */
    ngx_http_upstream_state_t       *state;

    ngx_str_t                        method;
    ngx_str_t                        schema;
    ngx_str_t                        uri;

#if (NGX_HTTP_SSL)
    ngx_str_t                        ssl_name;
#endif

    ngx_http_cleanup_pt             *cleanup;

    unsigned                         store:1;
    unsigned                         cacheable:1;
    unsigned                         accel:1;
    unsigned                         ssl:1;
#if (NGX_HTTP_CACHE)
    unsigned                         cache_status:3;
#endif

    /*
     * 读取上游数据的时候是否开启缓存buffer,如果不开启的话则从上游一单获取数据后会同步的输出到客户端,如果
     * 开启,则ngx会尽可能的将响应数据保存到缓存中,如果整个响应数据无法保存到缓存,则会将一部分保存到磁盘.
     *
     * 缓存的大小和块数一般有对应的_buffers指令(ngx_http_upstream_conf_t.bufs)
     *
     * proxy模块和fastcgi模块都有类似的指令来设置这个值,分别是proxy_buffering和fastcgi_buffering
     * 指令,并且默认都是开启的,
     *
     * 当buffering被禁用时,一旦读取到响应数据会立即发送给客户端,每次可以读到的最大数据量有_buffer_size
     * 指令设置(ngx_http_upstream_conf_t.buffer_size)
     */
    unsigned                         buffering:1;
    unsigned                         keepalive:1;
    unsigned                         upgrade:1;

    /*
     * 用来标记当前地址是否已经发送过请求
     *
     * 没有发送过请求时才会调用ngx_http_upstream_next()方法? TODO
     */
    unsigned                         request_sent:1;
    unsigned                         header_sent:1;
};


typedef struct {
    ngx_uint_t                      status;
    ngx_uint_t                      mask;
} ngx_http_upstream_next_t;


typedef struct {
    ngx_str_t   key;
    ngx_str_t   value;
    ngx_uint_t  skip_empty;
} ngx_http_upstream_param_t;


ngx_int_t ngx_http_upstream_cookie_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
ngx_int_t ngx_http_upstream_header_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

ngx_int_t ngx_http_upstream_create(ngx_http_request_t *r);
void ngx_http_upstream_init(ngx_http_request_t *r);
ngx_http_upstream_srv_conf_t *ngx_http_upstream_add(ngx_conf_t *cf,
    ngx_url_t *u, ngx_uint_t flags);
char *ngx_http_upstream_bind_set_slot(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
char *ngx_http_upstream_param_set_slot(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
ngx_int_t ngx_http_upstream_hide_headers_hash(ngx_conf_t *cf,
    ngx_http_upstream_conf_t *conf, ngx_http_upstream_conf_t *prev,
    ngx_str_t *default_hide_headers, ngx_hash_init_t *hash);


#define ngx_http_conf_upstream_srv_conf(uscf, module)                         \
    uscf->srv_conf[module.ctx_index]


extern ngx_module_t        ngx_http_upstream_module;
extern ngx_conf_bitmask_t  ngx_http_upstream_cache_method_mask[];
extern ngx_conf_bitmask_t  ngx_http_upstream_ignore_headers_masks[];


#endif /* _NGX_HTTP_UPSTREAM_H_INCLUDED_ */
