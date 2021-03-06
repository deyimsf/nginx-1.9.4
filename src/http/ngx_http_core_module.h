
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_CORE_H_INCLUDED_
#define _NGX_HTTP_CORE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_THREADS)
#include <ngx_thread_pool.h>
#endif


#define NGX_HTTP_GZIP_PROXIED_OFF       0x0002
#define NGX_HTTP_GZIP_PROXIED_EXPIRED   0x0004
#define NGX_HTTP_GZIP_PROXIED_NO_CACHE  0x0008
#define NGX_HTTP_GZIP_PROXIED_NO_STORE  0x0010
#define NGX_HTTP_GZIP_PROXIED_PRIVATE   0x0020
#define NGX_HTTP_GZIP_PROXIED_NO_LM     0x0040
#define NGX_HTTP_GZIP_PROXIED_NO_ETAG   0x0080
#define NGX_HTTP_GZIP_PROXIED_AUTH      0x0100
#define NGX_HTTP_GZIP_PROXIED_ANY       0x0200


#define NGX_HTTP_AIO_OFF                0
#define NGX_HTTP_AIO_ON                 1
#define NGX_HTTP_AIO_THREADS            2


#define NGX_HTTP_SATISFY_ALL            0
#define NGX_HTTP_SATISFY_ANY            1


#define NGX_HTTP_LINGERING_OFF          0
#define NGX_HTTP_LINGERING_ON           1
#define NGX_HTTP_LINGERING_ALWAYS       2


#define NGX_HTTP_IMS_OFF                0
#define NGX_HTTP_IMS_EXACT              1
#define NGX_HTTP_IMS_BEFORE             2


#define NGX_HTTP_KEEPALIVE_DISABLE_NONE    0x0002
#define NGX_HTTP_KEEPALIVE_DISABLE_MSIE6   0x0004
#define NGX_HTTP_KEEPALIVE_DISABLE_SAFARI  0x0008


typedef struct ngx_http_location_tree_node_s  ngx_http_location_tree_node_t;
typedef struct ngx_http_core_loc_conf_s  ngx_http_core_loc_conf_t;


/*
 * 代表一个server{}块下的一个监听地址,比如 listen 8080
 * 也就是说该结构体只能代表一个端口
 */
typedef struct {
    union {
        struct sockaddr        sockaddr;
        struct sockaddr_in     sockaddr_in;
#if (NGX_HAVE_INET6)
        struct sockaddr_in6    sockaddr_in6;
#endif
#if (NGX_HAVE_UNIX_DOMAIN)
        struct sockaddr_un     sockaddr_un;
#endif
        u_char                 sockaddr_data[NGX_SOCKADDRLEN];
    } u;

    socklen_t                  socklen;

    unsigned                   set:1;
    unsigned                   default_server:1;
    unsigned                   bind:1;
    unsigned                   wildcard:1;
#if (NGX_HTTP_SSL)
    unsigned                   ssl:1;
#endif
#if (NGX_HTTP_SPDY)
    unsigned                   spdy:1;
#endif
#if (NGX_HAVE_INET6 && defined IPV6_V6ONLY)
    unsigned                   ipv6only:1;
#endif
#if (NGX_HAVE_REUSEPORT)
    unsigned                   reuseport:1;
#endif
    unsigned                   so_keepalive:2;
    unsigned                   proxy_protocol:1;

    int                        backlog;
    int                        rcvbuf;
    int                        sndbuf;
#if (NGX_HAVE_SETFIB)
    int                        setfib;
#endif
#if (NGX_HAVE_TCP_FASTOPEN)
    int                        fastopen;
#endif
#if (NGX_HAVE_KEEPALIVE_TUNABLE)
    int                        tcp_keepidle;
    int                        tcp_keepintvl;
    int                        tcp_keepcnt;
#endif

#if (NGX_HAVE_DEFERRED_ACCEPT && defined SO_ACCEPTFILTER)
    char                      *accept_filter;
#endif
#if (NGX_HAVE_DEFERRED_ACCEPT && defined TCP_DEFER_ACCEPT)
    ngx_uint_t                 deferred_accept;
#endif

    u_char                     addr[NGX_SOCKADDR_STRLEN + 1];
} ngx_http_listen_opt_t;


typedef enum {
	/*
	 * 对应checker方法是ngx_http_core_generic_phase()
	 *
	 * 用到该阶段的模块有:
	 * 		/src/http/modules/ngx_http_realip_module.c
	 */
    NGX_HTTP_POST_READ_PHASE = 0,

	/*
	 * 对应checker方法是ngx_http_core_rewrite_phase()
	 * rewrite_module.c模块在该阶段注册的是ngx_http_rewrite_handler()方法
	 *
	 * 用到该阶段的模块有:
	 * 		/src/http/modules/ngx_http_rewrite_module.c
	 */
    NGX_HTTP_SERVER_REWRITE_PHASE,

	/*
	 * 不可以介入
	 * 对应checker方法是ngx_http_core_find_config_phase()
	 * 作用是匹配location
	 *
	 * 该阶段不可介入,所以不会有模块直接使用该阶段,该阶段直接有http核心模块的
	 * ngx_http_core_find_config_phase()方法负责执行
	 */
    NGX_HTTP_FIND_CONFIG_PHASE,

	/*
	 * 对应checker方法是ngx_http_core_rewrite_phase()
	 * rewrite_module.c模块在该阶段注册的是ngx_http_rewrite_handler()方法
	 *
	 * 用到该阶段的模块有:
	 * 		/src/http/modules/ngx_http_rewrite_module.c
	 */
    NGX_HTTP_REWRITE_PHASE,

	/*
	 * 不可以介入
	 * 对应checker方法是ngx_http_core_post_rewrite_phase()
	 * 如果uri有改变(r->uri_changed)则负责重新匹配location
	 *
	 * 该阶段不可介入,所以不会有模块直接使用该阶段,该阶段直接有http核心模块的
	 * ngx_http_core_post_rewrite_phase()方法负责执行
	 */
    NGX_HTTP_POST_REWRITE_PHASE,

	/*
	 * rewrite阶段执行完毕之后执行该阶段
	 * 对应checker方法是ngx_http_core_generic_phase()
	 *
	 * 用到该阶段的模块有:
	 * 		/src/http/modules/ngx_http_degradation_module.c
	 * 		/src/http/modules/ngx_http_limit_conn_module.c
	 * 		/src/http/modules/ngx_http_limit_req_module.c
	 * 		/src/http/modules/ngx_http_realip_module.c
	 */
    NGX_HTTP_PREACCESS_PHASE,

	/*
	 * 对应checker方法是ngx_http_core_access_phase()
	 *
	 * 用到该阶段的模块有:
	 * 		/src/http/modules/ngx_http_access_module.c
	 * 		/src/http/modules/ngx_http_auth_basic_module.c
	 * 		/src/http/modules/ngx_http_auth_request_module.c
	 */
    NGX_HTTP_ACCESS_PHASE,

	/*
	 * 不可介入
	 * 对应checker方法是ngx_http_core_post_access_phase()
	 *
	 * 该阶段不可介入,所以不会有模块直接使用该阶段,该阶段直接有http核心模块的
	 * ngx_http_core_post_access_phase()方法负责执行
	 */
    NGX_HTTP_POST_ACCESS_PHASE,

	/*
	 * 不可介入
	 * 对应checker方法是ngx_http_core_try_files_phase()
	 *
	 * 该阶段不可介入,所以不会有模块直接使用该阶段,该阶段直接有http核心模块的
	 * ngx_http_core_try_files_phase()方法负责执行
	 */
    NGX_HTTP_TRY_FILES_PHASE,

	/*
	 * 对应checker方法是ngx_http_core_content_phase()
	 *
	 * 用到该阶段的模块有(使用一般方式注册的handler):
	 *		/src/http/modules/ngx_http_autoindex_module.c
	 *		/src/http/modules/ngx_http_dav_module.c
	 *		/src/http/modules/ngx_http_gzip_static_module.c
	 *		/src/http/modules/ngx_http_index_module.c
	 *		/src/http/modules/ngx_http_random_index_module.c
	 *		/src/http/modules/ngx_http_static_module.c
	 */
    NGX_HTTP_CONTENT_PHASE,

	/*
	 * 不对应任何checker方法,该阶段不在阶段引擎中执行
	 * 当请求真正结束的时候会调用/src/http/ngx_http_request.c/ngx_http_log_request()方法
	 *
	 * 目前用到该阶段的模块有:
	 *	  /src/http/modules/ngx_http_log_module.c
	 */
    NGX_HTTP_LOG_PHASE
} ngx_http_phases;

typedef struct ngx_http_phase_handler_s  ngx_http_phase_handler_t;

typedef ngx_int_t (*ngx_http_phase_handler_pt)(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);

struct ngx_http_phase_handler_s {
    ngx_http_phase_handler_pt  checker;
    ngx_http_handler_pt        handler;

    // 下一个阶段的开发方法坐标
    ngx_uint_t                 next;
};


typedef struct {
    ngx_http_phase_handler_t  *handlers;

    // NGX_HTTP_SERVER_REWRITE_PHASE阶段在阶段引擎中的开始索引
    ngx_uint_t                 server_rewrite_index;

    // NGX_HTTP_REWRITE_PHASE阶段在阶段引擎中的开始索引
    ngx_uint_t                 location_rewrite_index;
} ngx_http_phase_engine_t;


/*
 * 该结构体代表http中的一个阶段
 * 每个阶段都包含一个handlers数组,该数组是每个阶段要执行的方法
 */
typedef struct {
	/*
	 * 存放http模块要执行的方法,目前方法签名是:
	 * 		typedef ngx_int_t (*ngx_http_handler_pt)(ngx_http_request_t *r);
	 *
	 * 在/src/http/ngx_http.c/ngx_http_init_phases()方法中初始化该数组
	 */
    ngx_array_t                handlers;
} ngx_http_phase_t;


/*
 * 整个ngx中只有一个该结构体
 * 可以认为其代表一个http{}块
 */
typedef struct {

	/*
	 * 存储http{}块下的所有server{}块配置项结构体(ngx_http_core_srv_conf_t)
	 * 当前http{}下有几个server{},就有几个ngx_http_core_srv_conf_t配置项结构体
	 */
    ngx_array_t                servers;         /* ngx_http_core_srv_conf_t */

    /*
     * 阶段引擎
     *
     *  typedef struct {
     *		ngx_http_phase_handler_t  *handlers;
     *		ngx_uint_t                 server_rewrite_index;
     *		ngx_uint_t                 location_rewrite_index;
	 *	} ngx_http_phase_engine_t;
     *
     */
    ngx_http_phase_engine_t    phase_engine;

    /*
     * 存放http请求头的hash结构
     */
    ngx_hash_t                 headers_in_hash;

    /*
     * 变量的hash结构,使用variables_keys->keys数组中的数据生成的
     */
    ngx_hash_t                 variables_hash;

    /*
     * 在第一次调用ngx_http_get_variable_index()方法的时候会为该字段分配内存空间
     *
     * set指令调用ngx_http_add_variable()和ngx_http_get_variable_index()方法
     * 分别把变量名放入到cmcf->variables_keys和cmcf->variables数组中
     *
     * 第三方模块一般也会通过ngx_http_add_variable()和ngx_http_get_variable_index()方法
     * 将自己的变量导入到ngx中
     *
     * 变量值存放在ngx_http_rewrite_loc_conf_t->codes中,最终有ngx_http_rewrite_handler()方法
     * 启动引擎来为r->variables中用到的变量赋值
     *
	 * 被使用的变量都在这里
	 * 被定义的变量都在variables_keys中
     */
    ngx_array_t                variables;       /* ngx_http_variable_t */
    ngx_uint_t                 ncaptures;

    ngx_uint_t                 server_names_hash_max_size;
    ngx_uint_t                 server_names_hash_bucket_size;

    ngx_uint_t                 variables_hash_max_size;
    ngx_uint_t                 variables_hash_bucket_size;


    /*
	 * set指令调用ngx_http_add_variable()和ngx_http_get_variable_index()方法
	 * 分别把变量名放入到cmcf->variables_keys和cmcf->variables数组中
	 *
	 * 键值对key是变量的名字
	 * 键值对value是ngx_http_variable_t结构体
	 *
	 * 存放ngx中的外置变量和内置变量(已定义变量)
	 * 它的作用基本就是防止变量重复定义,以及为variables中用到的内置变量设置get_handler方法
	 *
	 * 把variables字段设置完毕后,该字段内存就会被回收
	 *
	 * 被定义的变量都在这里
	 * 被使用的变量都在variables中
	 */
    ngx_hash_keys_arrays_t    *variables_keys;

    /*
     * ports:以端口维度保存的整个nginx的监听地址
	 * 		8080:{
	 * 			192.168.146.80:{
	 * 				www.jd.com
	 * 				d.jd.com
	 * 			}
	 *
	 *
	 * 			127.0.0.1:{
	 *				d.jd.com
	 *				www.jd.com
	 * 			}
	 * 		}
	 *
     */
    ngx_array_t               *ports;

    ngx_uint_t                 try_files;       /* unsigned  try_files:1 */

    /*
     * 存放所有http模块,在各个阶段要执行的所有方法
     *
     * 总共11个阶段NGX_HTTP_LOG_PHASE在枚举ngx_http_phases中的值是10
     * 这里数组的长度设置为11
     *
     * 该字段只是用来收集注册方法
     */
    ngx_http_phase_t           phases[NGX_HTTP_LOG_PHASE + 1];
} ngx_http_core_main_conf_t;


/*
 * 代表一个server{}块
 *
 * server{}块下的location都放到哪里了呢? ngx为了节省资源直接利用了下面的结构体
 *    ngx_http_core_loc_conf_t clcf = ngx_http_get_module_loc_conf(ctx, ngx_http_core_module)
 * http核心模块在server块也会放一个loc_conf_t级别的结构体,而这个结构体中又有一个字段
 *    *locations
 * ngx使用该字段来收集server块下的location,从ngx_http_core_location()方法中可以看到
 *    pclcf = pctx->loc_conf[ngx_http_core_module.ctx_index];
 * 先是从上下文中取出http核心模块的loc级别结构体,然后在调用如下的方法
 *    ngx_http_add_location(cf, &pclcf->locations, clcf)
 * 把代表当前location的结构体clcf放入到父级别结构体(pclcf)对应的locations字段中
 *
 */

typedef struct {
    /* array of the ngx_http_server_name_t, "server_name" directive */
	/*
	 * 当前server{}下的所有域名,有server_name指令提供
	 */
    ngx_array_t                 server_names;

    /* server ctx */
    /* 主要用来存储所有http模块，在server{}块的配置项结构体(各个自定义模块的srv_conf和loc_conf结构体)
     * 当然包括核心http模块的ngx_http_core_srv|loc_conf结构体
     * 用到了 **srv_conf和**loc_conf
     */
    ngx_http_conf_ctx_t        *ctx;

    ngx_str_t                   server_name;

    size_t                      connection_pool_size;
    size_t                      request_pool_size;
    size_t                      client_header_buffer_size;

    ngx_bufs_t                  large_client_header_buffers;

    ngx_msec_t                  client_header_timeout;

    ngx_flag_t                  ignore_invalid_headers;
    ngx_flag_t                  merge_slashes;
    ngx_flag_t                  underscores_in_headers;

    unsigned                    listen:1;
#if (NGX_PCRE)
    unsigned                    captures:1;
#endif

    /*
     *  存放@匹配的location
     */
    ngx_http_core_loc_conf_t  **named_locations;
} ngx_http_core_srv_conf_t;


/* list of structures to find core_srv_conf quickly at run time */


typedef struct {
#if (NGX_PCRE)
    ngx_http_regex_t          *regex;
#endif
    ngx_http_core_srv_conf_t  *server;   /* virtual name server conf */
    ngx_str_t                  name;
} ngx_http_server_name_t;


typedef struct {
     ngx_hash_combined_t       names;

     ngx_uint_t                nregex;
     ngx_http_server_name_t   *regex;
} ngx_http_virtual_names_t;


struct ngx_http_addr_conf_s {
    /* the default server configuration for this address:port */
    ngx_http_core_srv_conf_t  *default_server;

    ngx_http_virtual_names_t  *virtual_names;

#if (NGX_HTTP_SSL)
    unsigned                   ssl:1;
#endif
#if (NGX_HTTP_SPDY)
    unsigned                   spdy:1;
#endif
    unsigned                   proxy_protocol:1;
};


typedef struct {
    in_addr_t                  addr;
    ngx_http_addr_conf_t       conf;
} ngx_http_in_addr_t;


#if (NGX_HAVE_INET6)

typedef struct {
    struct in6_addr            addr6;
    ngx_http_addr_conf_t       conf;
} ngx_http_in6_addr_t;

#endif


/*
 * 代表一个端口,跟ngx_http_conf_port_t的区别是,ngx_http_conf_port_t是一个配置信息,
 * 他里面包含了当前nginx关于监听地址的所有配置信息(ip、port、域名)。
 * 而该结构体是被每个ngx_listening_t引用的,每一个ngx_listening_t对象,都会用servers来
 * 指定一个ngx_http_port_t,也就是说ngx_listening_t在端口维度引用了所有的监听地址。
 */
typedef struct {
    /* ngx_http_in_addr_t or ngx_http_in6_addr_t */
	// 端口下的所有地址
    void                      *addrs;
    // 端口下地址的个数
    ngx_uint_t                 naddrs;
} ngx_http_port_t;


/*
 * 代表一个端口,端口维度
 * 如下配置:
 * 	 server { // server1
 * 	 	listen		127.0.0.1:8080;
 * 	 	listen		127.0.0.2:8080;
 * 	 	server_name host1 host2;
 * 	 }
 *
 * 	 server { // server2
 * 	 	listen		127.0.0.1:8080;
 * 	 	listen		127.0.0.2:8080;
 * 	 	server_name host3 host4;
 * 	 }
 * 这样一个配置会产生一个ngx_http_conf_port_t结构体,因为端口都一样
 *
 * ngx_http_conf_port_t->addrs: 8080
 * 		ngx_http_conf_addr_t->servers: 127.0.0.1
 *			ngx_http_core_srv_conf_t->server_names: server1
 *				host1
 *				host2
 *			ngx_http_core_srv_conf_t->server_names: server2
 *				host3
 *				host4
 *
 * 		ngx_http_conf_addr_t->servers: 127.0.0.2
 * 			ngx_http_core_srv_conf_t: server1
 * 				host1
 *				host2
 *			ngx_http_core_srv_conf_t: server2
 *			 	host3
 *				host4
 *
 */
typedef struct {
    ngx_int_t                  family;

    /*
     * 端口号
     */
    in_port_t                  port;
    /*
     * 某个端口号下的地址,比如
     * 	80:
     * 		www.jd.com
     * 		www.baidu.com
     *  8080:
     *		127.0.0.1
     *		aaa.jd.com
     */
    ngx_array_t                addrs;     /* array of ngx_http_conf_addr_t */
} ngx_http_conf_port_t;


/*
 * 代表某个端口下的一个地址,所以该结构体是ip地址维度的
 *
 * 如下配置:
 * 	 server { // server1
 * 	 	listen		127.0.0.1:8080;
 * 	 	listen		127.0.0.2:8080;
 * 	 	server_name host1 host2;
 * 	 }
 *
 * 	 server { // server2
 * 	 	listen		127.0.0.1:8080;
 * 	 	listen		127.0.0.2:8080;
 * 	 	server_name host3 host4;
 * 	 }
 * 这样一个配置,对于8080端口会产生两个该结构体,分别是:
 *    代表127.0.0.1:8080地址的
 *    代表127.0.0.2:8080地址的
 *
 * 该结构体中的servers数组,会存放代表server1和server2的ngx_http_core_srv_conf_t结构体
 *
 *
 * ngx_http_conf_addr_t --> 127.0.0.1
 *		servers:
 *			----------------------------------------------------
 *			|ngx_http_core_srv_conf_t|ngx_http_core_srv_conf_t|
 *			----------------------------------------------------
 *
 * ngx_http_conf_addr_t --> 127.0.0.2
 *  	servers:
 *			----------------------------------------------------
 *			|ngx_http_core_srv_conf_t|ngx_http_core_srv_conf_t|
 *			----------------------------------------------------
 */
typedef struct {
    ngx_http_listen_opt_t      opt;

    /*
     * 简易hash结构,不带通配符的(如: www.jd.com)
     */
    ngx_hash_t                 hash;
    /*
     * 通配符的hash结构,存放前置通配符域名(*.jd.com)
     */
    ngx_hash_wildcard_t       *wc_head;
    /*
     * 通配符的hash结构,存放后置通配符域名(www.jd.*)
     */
    ngx_hash_wildcard_t       *wc_tail;

#if (NGX_PCRE)
    ngx_uint_t                 nregex;
    /*
     * 存放正则匹配的域名(如: www.jd.{1,3}.com)
     */
    ngx_http_server_name_t    *regex;
#endif

    /* the default server configuration for this address:port */
    ngx_http_core_srv_conf_t  *default_server;

    /*
     * 某个端口下面的所有server{}块  一个server{}块下面可以有多个端口
     * 如下配置:
     * 	 server { // server1
     * 	 	listen		127.0.0.1:8080;
     * 	 	listen		127.0.0.2:8080;
     * 	 	server_name host1 host2;
     * 	 }
     *
     * 	 server { // server2
     * 	 	listen		127.0.0.1:8080;
     * 	 	listen		127.0.0.2:8080;
     * 	 	server_name host3 host4;
     * 	 }
     * 则对于127.0.0.1:8080这个地址,servers数组中存放了两个server{}块,分别是server1和server2
     *
     */
    ngx_array_t                servers;  /* array of ngx_http_core_srv_conf_t */
} ngx_http_conf_addr_t;


typedef struct {
    ngx_int_t                  status;
    ngx_int_t                  overwrite;
    ngx_http_complex_value_t   value;
    ngx_str_t                  args;
} ngx_http_err_page_t;


typedef struct {
    ngx_array_t               *lengths;
    ngx_array_t               *values;
    ngx_str_t                  name;

    unsigned                   code:10;
    unsigned                   test_dir:1;
} ngx_http_try_file_t;


/*
 * 可以认为该结构体代表一个location{}块
 */
struct ngx_http_core_loc_conf_s {
	/*
	 * location指令对应的名字,比如：
	 * 		location ~  /aa {}
	 * 那么该值就是 /aa
	 */
    ngx_str_t     name;          /* location name */

#if (NGX_PCRE)
    /*
     * 编译好的正则,同时也可以表示带正则的location,比如:
     * 		location ~ /aa {}
     */
    ngx_http_regex_t  *regex;
#endif

    /*
     * if () {}: ngx中,该指令被看成一个location{}块,把自己放入到locations队列中
     * 			 看/src/http/modules/ngx_http_rewrite_module.c/ngx_http_rewrite_if()方法
     *
     * limit_except method {}: ngx中,该指令被看成一个location{}块,把自己放入到locations队列中
     * 			看/src/http/ngx_http_core_module.c/ngx_http_core_limit_except
     */
    unsigned      noname:1;   /* "if () {}" block or limit_except */
    unsigned      lmt_excpt:1;
    /*
     * 值是1:前缀是@的location:
     * 	location @abc {}
     */
    unsigned      named:1;

    /*
     * 值是1:表示精确匹配:
     *   location = /abc {}
     */
    unsigned      exact_match:1;
    /*
     * 没有编译正则表达式?:
     * 	 location ^~ /abc {}
     */
    unsigned      noregex:1;

    /**
     * 用来完成自动重定向规则的一个钩子,如果在解析某个location下的指令时，有指令把该字段设置为1，
     * 那么，当该location后缀有“/”时，如果对应的请求不带“/”，则nginx会用一个301让其补上一个“/”
     * 后缀后再请求。
     *
     * 这种规则只在【无】【^~】【=】这三种修饰符中出现，因为它是在find_static_location()方法中完成的
     *
     * 这个机制在ngx_http_core_find_static_location()方法中体现,用下面代码完成:
     *       if (len + 1 == (size_t) node->len && node->auto_redirect) {
     *           r->loc_conf = (node->exact) ? node->exact->loc_conf:     // locaiton = /a/ {}
     *                                         node->inclusive->loc_conf; // location /a/ {}
     *           rv = NGX_DONE;
     *       }
     * 首先，能走到这里，表示uri的名字和node->name的名字是有重合的，要么是uri的名字子
     * 包含node->name的名字(比如,uri="/abc",node->name="/ab")，要么是node->name的名字
     * 包含uri的名字(比如,node->name="/abc",uri="/ab").
     * 然后如果node->auto_redirect被标记为1，那么表示该node->name的最后一个字符肯定是"/"，
     * 如果uri的名字的长度加一正好是node->name名字的长度，那就说明uri的名字只比node->name
     * 少一个"/"符号，至此auto_redirect生效，并返回NGX_DONE，
     * 然后它的最上层方法ngx_http_core_find_config_phase()
     *   该方法会根据NGX_DONE标记并通过ngx_http_finalize_request(r, NGX_HTTP_MOVED_PERMANENTLY)方法来结束请求。
     *   而最后Location响应头中的“/”是通过clcf->name加上的，具体有两种情况:
     *   1.请求"/a"后没有查询参数，则直接赋值
     *     r->headers_out.location->value = clcf->name;
     *   2.请求"/a?name=1"后有查询参数，则如下方式赋值:
     *     r->headers.out.location->value = clcf->name + "?" + "name=1"
     *
     */
    unsigned      auto_redirect:1;
#if (NGX_HTTP_GZIP)
    unsigned      gzip_disable_msie6:2;
#if (NGX_HTTP_DEGRADATION)
    unsigned      gzip_disable_degradation:2;
#endif
#endif

    /*
     * 含精确匹配(=)和无符号(^~|无符号)匹配的location块
     */
    ngx_http_location_tree_node_t   *static_locations;
#if (NGX_PCRE)
    // 存放正则匹配的location
    ngx_http_core_loc_conf_t       **regex_locations;
#endif

    /*
     * pointer to the modules' loc_conf
     *
     *
     * 在某个location{}块内的所有http模块的loc_conf配置项结构体
     * locl_conf[module.ctx_index]
     */
    void        **loc_conf;

    uint32_t      limit_except;
    /*
     * TODO 该字段和loc_conf字段的关系
     */
    void        **limit_except_loc_conf;

    // CONTENT_PHASE阶段的handler回调函数
    ngx_http_handler_pt  handler;

    /* location name length for inclusive location with inherited alias */
    size_t        alias;
    ngx_str_t     root;                    /* root, alias */
    ngx_str_t     post_action;

    ngx_array_t  *root_lengths;
    ngx_array_t  *root_values;

    ngx_array_t  *types;
    ngx_hash_t    types_hash;
    ngx_str_t     default_type;

    off_t         client_max_body_size;    /* client_max_body_size */
    off_t         directio;                /* directio */
    off_t         directio_alignment;      /* directio_alignment */

    size_t        client_body_buffer_size; /* client_body_buffer_size */
    size_t        send_lowat;              /* send_lowat */
    size_t        postpone_output;         /* postpone_output */
    size_t        limit_rate;              /* limit_rate */
    size_t        limit_rate_after;        /* limit_rate_after */
    size_t        sendfile_max_chunk;      /* sendfile_max_chunk */
    size_t        read_ahead;              /* read_ahead */

    ngx_msec_t    client_body_timeout;     /* client_body_timeout */
    ngx_msec_t    send_timeout;            /* send_timeout */
    ngx_msec_t    keepalive_timeout;       /* keepalive_timeout */
    ngx_msec_t    lingering_time;          /* lingering_time */
    ngx_msec_t    lingering_timeout;       /* lingering_timeout */
    ngx_msec_t    resolver_timeout;        /* resolver_timeout */

    ngx_resolver_t  *resolver;             /* resolver */

    time_t        keepalive_header;        /* keepalive_timeout */

    ngx_uint_t    keepalive_requests;      /* keepalive_requests */
    ngx_uint_t    keepalive_disable;       /* keepalive_disable */
    ngx_uint_t    satisfy;                 /* satisfy */
    ngx_uint_t    lingering_close;         /* lingering_close */
    ngx_uint_t    if_modified_since;       /* if_modified_since */
    ngx_uint_t    max_ranges;              /* max_ranges */
    ngx_uint_t    client_body_in_file_only; /* client_body_in_file_only */

    ngx_flag_t    client_body_in_single_buffer;
                                           /* client_body_in_singe_buffer */

    // location{}块中的internal指令
    ngx_flag_t    internal;                /* internal */
    ngx_flag_t    sendfile;                /* sendfile */
    ngx_flag_t    aio;                     /* aio */
    ngx_flag_t    tcp_nopush;              /* tcp_nopush */
    ngx_flag_t    tcp_nodelay;             /* tcp_nodelay */
    ngx_flag_t    reset_timedout_connection; /* reset_timedout_connection */
    ngx_flag_t    server_name_in_redirect; /* server_name_in_redirect */
    ngx_flag_t    port_in_redirect;        /* port_in_redirect */
    ngx_flag_t    msie_padding;            /* msie_padding */
    ngx_flag_t    msie_refresh;            /* msie_refresh */
    ngx_flag_t    log_not_found;           /* log_not_found */
    ngx_flag_t    log_subrequest;          /* log_subrequest */
    ngx_flag_t    recursive_error_pages;   /* recursive_error_pages */
    ngx_flag_t    server_tokens;           /* server_tokens */
    ngx_flag_t    chunked_transfer_encoding; /* chunked_transfer_encoding */
    ngx_flag_t    etag;                    /* etag */

#if (NGX_HTTP_GZIP)
    ngx_flag_t    gzip_vary;               /* gzip_vary */

    ngx_uint_t    gzip_http_version;       /* gzip_http_version */
    ngx_uint_t    gzip_proxied;            /* gzip_proxied */

#if (NGX_PCRE)
    ngx_array_t  *gzip_disable;            /* gzip_disable */
#endif
#endif

#if (NGX_THREADS)
    ngx_thread_pool_t         *thread_pool;
    ngx_http_complex_value_t  *thread_pool_value;
#endif

#if (NGX_HAVE_OPENAT)
    ngx_uint_t    disable_symlinks;        /* disable_symlinks */
    ngx_http_complex_value_t  *disable_symlinks_from;
#endif

    ngx_array_t  *error_pages;             /* error_page */
    ngx_http_try_file_t    *try_files;     /* try_files */

    ngx_path_t   *client_body_temp_path;   /* client_body_temp_path */

    ngx_open_file_cache_t  *open_file_cache;
    time_t        open_file_cache_valid;
    ngx_uint_t    open_file_cache_min_uses;
    ngx_flag_t    open_file_cache_errors;
    ngx_flag_t    open_file_cache_events;

    ngx_log_t    *error_log;

    ngx_uint_t    types_hash_max_size;
    ngx_uint_t    types_hash_bucket_size;

    /*
     * 一个ngx_http_core_loc_conf_t结构体可以认为代表一个location{}
	 *
     * 如果当前结构体是在server{}中的ngx_http_conf_ctx_t->loc_conf[ngx_http_core_module.ctx_index]
     * 那么locations就是server{}块下所属的所有location{}块
     *
     * 如果当前结构体是在location{}中的ngx_http_conf_ctx_t->loc_conf[ngx_http_core_module.ctx_index]
     * 那么locations就是location{}块下所属的所有location{}块
     *
     *
     * locations队列排完序后的顺序如下:
     * 	0.在/src/http/ngx_http_core_module.c/ngx_http_core_find_config_phase()阶段方法中匹配location
     *
	 *  1.=(精确匹配,终止匹配) | 无修饰符(不终止匹配) | ^~ (和无修饰符相同,但终止匹配)
	 *     按字符倒序排序。在NGX_HTTP_FIND_CONFIG_PHASE阶段执行匹配工作。
	 *
	 *  2.~* | ~
	 *     按照在配置文件中的位置排序,终止匹配。在NGX_HTTP_FIND_CONFIG_PHASE阶段执行匹配工作。
	 *
	 *  3.@
	 *     内部匹配执行,匹配完后就执行,对应/src/http/ngx_http_core_module.c/ngx_http_named_location()方法
	 *     不参与NGX_HTTP_FIND_CONFIG_PHASE阶段的匹配工作。
	 *
	 *  4.if () {}
	 *  	rewirte模块的脚本引擎执行,不参与NGX_HTTP_FIND_CONFIG_PHASE阶段的匹配工作
	 *
	 *  最后会被拆分开成只剩下上面的第一种
     */
    ngx_queue_t  *locations;

#if 0
    ngx_http_core_loc_conf_t  *prev_location;
#endif
};


/*
 * 一个队列元素
 * 存放ngx_http_core_loc_conf_t对象
 */
typedef struct {
    ngx_queue_t                      queue;
    /*
	 * 1.如果location是精确匹配(=)
	 * 2.如果location是正则匹配(~ or ~*)
	 * 3.如果location是@匹配
	 */
    ngx_http_core_loc_conf_t        *exact;
    /*
     * 如果location是一般匹配(无修饰符 or ^~)
     */
    ngx_http_core_loc_conf_t        *inclusive;
    // location名字
    ngx_str_t                       *name;
    // location所在文件的名字
    u_char                          *file_name;
    // location所在的行
    ngx_uint_t                       line;

    /*
	 * TODO ?
	 * 放嵌套的locatio,比如:
	 * 		location /a {
	 * 			locaiton /a/b {}
	 * 			location /a/c {}
	 * 			location /a/d {}
	 * 		}
	 * 则list中存放的就是/a里面的location,最后会用他来组装ngx_http_location_tree_node_s中的tree字段
	 *
	 */
    ngx_queue_t                      list;
} ngx_http_location_queue_t;


struct ngx_http_location_tree_node_s {
	// 左子树
    ngx_http_location_tree_node_t   *left;
    // 右子树
    ngx_http_location_tree_node_t   *right;
    /*
     * TODO ?
     * 放嵌套的locatio,比如:
     * 		location /a {
     * 			locaiton /a/b {}
     * 			location /a/c {}
     * 			location /a/d {}
     * 		}
     * 则tree中存放的就是/a里面的location,并且名字会去掉/a前缀
     *
     */
    ngx_http_location_tree_node_t   *tree;

    ngx_http_core_loc_conf_t        *exact;
    ngx_http_core_loc_conf_t        *inclusive;

    /**
     * 用来完成自动重定向规则的一个钩子,该值来自于ngx_http_core_loc_conf_t结构体中的同名字段
     */
    u_char                           auto_redirect;
    u_char                           len;
    u_char                           name[1];
};


void ngx_http_core_run_phases(ngx_http_request_t *r);
ngx_int_t ngx_http_core_generic_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
ngx_int_t ngx_http_core_rewrite_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
ngx_int_t ngx_http_core_find_config_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
ngx_int_t ngx_http_core_post_rewrite_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
ngx_int_t ngx_http_core_access_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
ngx_int_t ngx_http_core_post_access_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
ngx_int_t ngx_http_core_try_files_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);
ngx_int_t ngx_http_core_content_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);


void *ngx_http_test_content_type(ngx_http_request_t *r, ngx_hash_t *types_hash);
ngx_int_t ngx_http_set_content_type(ngx_http_request_t *r);
void ngx_http_set_exten(ngx_http_request_t *r);
ngx_int_t ngx_http_set_etag(ngx_http_request_t *r);
void ngx_http_weak_etag(ngx_http_request_t *r);
ngx_int_t ngx_http_send_response(ngx_http_request_t *r, ngx_uint_t status,
    ngx_str_t *ct, ngx_http_complex_value_t *cv);
u_char *ngx_http_map_uri_to_path(ngx_http_request_t *r, ngx_str_t *name,
    size_t *root_length, size_t reserved);
ngx_int_t ngx_http_auth_basic_user(ngx_http_request_t *r);
#if (NGX_HTTP_GZIP)
ngx_int_t ngx_http_gzip_ok(ngx_http_request_t *r);
#endif


/*
 * 发起一个子请求
 *
 *
 */
ngx_int_t ngx_http_subrequest(ngx_http_request_t *r,
    ngx_str_t *uri, ngx_str_t *args, ngx_http_request_t **sr,
    ngx_http_post_subrequest_t *psr, ngx_uint_t flags);
ngx_int_t ngx_http_internal_redirect(ngx_http_request_t *r,
    ngx_str_t *uri, ngx_str_t *args);
ngx_int_t ngx_http_named_location(ngx_http_request_t *r, ngx_str_t *name);


ngx_http_cleanup_t *ngx_http_cleanup_add(ngx_http_request_t *r, size_t size);


typedef ngx_int_t (*ngx_http_output_header_filter_pt)(ngx_http_request_t *r);
typedef ngx_int_t (*ngx_http_output_body_filter_pt)
    (ngx_http_request_t *r, ngx_chain_t *chain);
typedef ngx_int_t (*ngx_http_request_body_filter_pt)
    (ngx_http_request_t *r, ngx_chain_t *chain);


ngx_int_t ngx_http_output_filter(ngx_http_request_t *r, ngx_chain_t *chain);
ngx_int_t ngx_http_write_filter(ngx_http_request_t *r, ngx_chain_t *chain);
ngx_int_t ngx_http_request_body_save_filter(ngx_http_request_t *r,
   ngx_chain_t *chain);


ngx_int_t ngx_http_set_disable_symlinks(ngx_http_request_t *r,
    ngx_http_core_loc_conf_t *clcf, ngx_str_t *path, ngx_open_file_info_t *of);

ngx_int_t ngx_http_get_forwarded_addr(ngx_http_request_t *r, ngx_addr_t *addr,
    ngx_array_t *headers, ngx_str_t *value, ngx_array_t *proxies,
    int recursive);


extern ngx_module_t  ngx_http_core_module;

extern ngx_uint_t ngx_http_max_module;

extern ngx_str_t  ngx_http_core_get_method;


#define ngx_http_clear_content_length(r)                                      \
                                                                              \
    r->headers_out.content_length_n = -1;                                     \
    if (r->headers_out.content_length) {                                      \
        r->headers_out.content_length->hash = 0;                              \
        r->headers_out.content_length = NULL;                                 \
    }

#define ngx_http_clear_accept_ranges(r)                                       \
                                                                              \
    r->allow_ranges = 0;                                                      \
    if (r->headers_out.accept_ranges) {                                       \
        r->headers_out.accept_ranges->hash = 0;                               \
        r->headers_out.accept_ranges = NULL;                                  \
    }

#define ngx_http_clear_last_modified(r)                                       \
                                                                              \
    r->headers_out.last_modified_time = -1;                                   \
    if (r->headers_out.last_modified) {                                       \
        r->headers_out.last_modified->hash = 0;                               \
        r->headers_out.last_modified = NULL;                                  \
    }

#define ngx_http_clear_location(r)                                            \
                                                                              \
    if (r->headers_out.location) {                                            \
        r->headers_out.location->hash = 0;                                    \
        r->headers_out.location = NULL;                                       \
    }

#define ngx_http_clear_etag(r)                                                \
                                                                              \
    if (r->headers_out.etag) {                                                \
        r->headers_out.etag->hash = 0;                                        \
        r->headers_out.etag = NULL;                                           \
    }


#endif /* _NGX_HTTP_CORE_H_INCLUDED_ */
