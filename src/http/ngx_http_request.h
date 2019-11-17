
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_REQUEST_H_INCLUDED_
#define _NGX_HTTP_REQUEST_H_INCLUDED_


#define NGX_HTTP_MAX_URI_CHANGES           10
#define NGX_HTTP_MAX_SUBREQUESTS           200

/* must be 2^n */
#define NGX_HTTP_LC_HEADER_LEN             32


#define NGX_HTTP_DISCARD_BUFFER_SIZE       4096
#define NGX_HTTP_LINGERING_BUFFER_SIZE     4096


#define NGX_HTTP_VERSION_9                 9
#define NGX_HTTP_VERSION_10                1000
#define NGX_HTTP_VERSION_11                1001

#define NGX_HTTP_UNKNOWN                   0x0001
#define NGX_HTTP_GET                       0x0002
#define NGX_HTTP_HEAD                      0x0004
#define NGX_HTTP_POST                      0x0008
#define NGX_HTTP_PUT                       0x0010
#define NGX_HTTP_DELETE                    0x0020
#define NGX_HTTP_MKCOL                     0x0040
#define NGX_HTTP_COPY                      0x0080
#define NGX_HTTP_MOVE                      0x0100
#define NGX_HTTP_OPTIONS                   0x0200
#define NGX_HTTP_PROPFIND                  0x0400
#define NGX_HTTP_PROPPATCH                 0x0800
#define NGX_HTTP_LOCK                      0x1000
#define NGX_HTTP_UNLOCK                    0x2000
#define NGX_HTTP_PATCH                     0x4000
#define NGX_HTTP_TRACE                     0x8000

#define NGX_HTTP_CONNECTION_CLOSE          1
#define NGX_HTTP_CONNECTION_KEEP_ALIVE     2


#define NGX_NONE                           1


#define NGX_HTTP_PARSE_HEADER_DONE         1

#define NGX_HTTP_CLIENT_ERROR              10
#define NGX_HTTP_PARSE_INVALID_METHOD      10
#define NGX_HTTP_PARSE_INVALID_REQUEST     11
#define NGX_HTTP_PARSE_INVALID_09_METHOD   12

#define NGX_HTTP_PARSE_INVALID_HEADER      13


/* unused                                  1 */
/*
 * Output is not sent to the client, but rather stored in memory.The flag only affects
 * subrequests which are processed by one of the proxying modules. After a subrequest is
 * finalized its output is available in a r->upstream->buffer of type ngx_buf_t.
 */
#define NGX_HTTP_SUBREQUEST_IN_MEMORY      2
/*
 * The subrequest's done flag is set even if the subrequest is not active when it is finalized.
 * This subrequest flag is used by the SSI filter.
 */
#define NGX_HTTP_SUBREQUEST_WAITED         4
#define NGX_HTTP_LOG_UNSAFE                8


#define NGX_HTTP_CONTINUE                  100
#define NGX_HTTP_SWITCHING_PROTOCOLS       101
#define NGX_HTTP_PROCESSING                102

#define NGX_HTTP_OK                        200
#define NGX_HTTP_CREATED                   201
#define NGX_HTTP_ACCEPTED                  202
#define NGX_HTTP_NO_CONTENT                204
#define NGX_HTTP_PARTIAL_CONTENT           206

#define NGX_HTTP_SPECIAL_RESPONSE          300
#define NGX_HTTP_MOVED_PERMANENTLY         301
#define NGX_HTTP_MOVED_TEMPORARILY         302
#define NGX_HTTP_SEE_OTHER                 303
#define NGX_HTTP_NOT_MODIFIED              304
#define NGX_HTTP_TEMPORARY_REDIRECT        307

#define NGX_HTTP_BAD_REQUEST               400
#define NGX_HTTP_UNAUTHORIZED              401
#define NGX_HTTP_FORBIDDEN                 403
#define NGX_HTTP_NOT_FOUND                 404
#define NGX_HTTP_NOT_ALLOWED               405
#define NGX_HTTP_REQUEST_TIME_OUT          408
#define NGX_HTTP_CONFLICT                  409
#define NGX_HTTP_LENGTH_REQUIRED           411
#define NGX_HTTP_PRECONDITION_FAILED       412
#define NGX_HTTP_REQUEST_ENTITY_TOO_LARGE  413
#define NGX_HTTP_REQUEST_URI_TOO_LARGE     414
#define NGX_HTTP_UNSUPPORTED_MEDIA_TYPE    415
#define NGX_HTTP_RANGE_NOT_SATISFIABLE     416


/* Our own HTTP codes */

/* The special code to close connection without any response */
#define NGX_HTTP_CLOSE                     444

#define NGX_HTTP_NGINX_CODES               494

#define NGX_HTTP_REQUEST_HEADER_TOO_LARGE  494

#define NGX_HTTPS_CERT_ERROR               495
#define NGX_HTTPS_NO_CERT                  496

/*
 * We use the special code for the plain HTTP requests that are sent to
 * HTTPS port to distinguish it from 4XX in an error page redirection
 */
#define NGX_HTTP_TO_HTTPS                  497

/* 498 is the canceled code for the requests with invalid host name */

/*
 * HTTP does not define the code for the case when a client closed
 * the connection while we are processing its request so we introduce
 * own code to log such situation when a client has closed the connection
 * before we even try to send the HTTP header to it
 */
#define NGX_HTTP_CLIENT_CLOSED_REQUEST     499


#define NGX_HTTP_INTERNAL_SERVER_ERROR     500
#define NGX_HTTP_NOT_IMPLEMENTED           501
#define NGX_HTTP_BAD_GATEWAY               502
#define NGX_HTTP_SERVICE_UNAVAILABLE       503
#define NGX_HTTP_GATEWAY_TIME_OUT          504
#define NGX_HTTP_INSUFFICIENT_STORAGE      507


#define NGX_HTTP_LOWLEVEL_BUFFERED         0xf0
#define NGX_HTTP_WRITE_BUFFERED            0x10
#define NGX_HTTP_GZIP_BUFFERED             0x20
#define NGX_HTTP_SSI_BUFFERED              0x01
#define NGX_HTTP_SUB_BUFFERED              0x02
#define NGX_HTTP_COPY_BUFFERED             0x04


typedef enum {
    NGX_HTTP_INITING_REQUEST_STATE = 0,
    NGX_HTTP_READING_REQUEST_STATE,
    NGX_HTTP_PROCESS_REQUEST_STATE,

    NGX_HTTP_CONNECT_UPSTREAM_STATE,
    NGX_HTTP_WRITING_UPSTREAM_STATE,
    NGX_HTTP_READING_UPSTREAM_STATE,

    NGX_HTTP_WRITING_REQUEST_STATE,
    NGX_HTTP_LINGERING_CLOSE_STATE,
    NGX_HTTP_KEEPALIVE_STATE
} ngx_http_state_e;


/*
 * 代表一个http请求头
 */
typedef struct {
	// 请求头名字
    ngx_str_t                         name;
    ngx_uint_t                        offset;

    /*
     * 获取请求头值的方法
     * 如: /src/http/ngx_http_request.c/ngx_http_headers_in()
     *     /src/http/ngx_http_request.c/ngx_http_process_host()
     */
    ngx_http_header_handler_pt        handler;
} ngx_http_header_t;


typedef struct {
    ngx_str_t                         name;
    ngx_uint_t                        offset;
} ngx_http_header_out_t;


typedef struct {
    ngx_list_t                        headers;

    ngx_table_elt_t                  *host;
    ngx_table_elt_t                  *connection;
    ngx_table_elt_t                  *if_modified_since;
    ngx_table_elt_t                  *if_unmodified_since;
    ngx_table_elt_t                  *if_match;
    ngx_table_elt_t                  *if_none_match;
    ngx_table_elt_t                  *user_agent;
    ngx_table_elt_t                  *referer;
    ngx_table_elt_t                  *content_length;
    ngx_table_elt_t                  *content_type;

    ngx_table_elt_t                  *range;
    ngx_table_elt_t                  *if_range;

    ngx_table_elt_t                  *transfer_encoding;
    ngx_table_elt_t                  *expect;
    ngx_table_elt_t                  *upgrade;

#if (NGX_HTTP_GZIP)
    ngx_table_elt_t                  *accept_encoding;
    ngx_table_elt_t                  *via;
#endif

    ngx_table_elt_t                  *authorization;

    ngx_table_elt_t                  *keep_alive;

#if (NGX_HTTP_X_FORWARDED_FOR)
    ngx_array_t                       x_forwarded_for;
#endif

#if (NGX_HTTP_REALIP)
    ngx_table_elt_t                  *x_real_ip;
#endif

#if (NGX_HTTP_HEADERS)
    ngx_table_elt_t                  *accept;
    ngx_table_elt_t                  *accept_language;
#endif

#if (NGX_HTTP_DAV)
    ngx_table_elt_t                  *depth;
    ngx_table_elt_t                  *destination;
    ngx_table_elt_t                  *overwrite;
    ngx_table_elt_t                  *date;
#endif

    ngx_str_t                         user;
    ngx_str_t                         passwd;

    ngx_array_t                       cookies;

    ngx_str_t                         server;
    off_t                             content_length_n;
    time_t                            keep_alive_n;

    unsigned                          connection_type:2;
    unsigned                          chunked:1;
    unsigned                          msie:1;
    unsigned                          msie6:1;
    unsigned                          opera:1;
    unsigned                          gecko:1;
    unsigned                          chrome:1;
    unsigned                          safari:1;
    unsigned                          konqueror:1;
} ngx_http_headers_in_t;


/*
 * 存放响应头的结构体
 * 由/src/http/ngx_http_header_filter_module.c模块输出
 */
typedef struct {
	/*
	 * To output an arbitrary header, append the headers list.
	 *
	 * 存放和输出自定义的响应头
	 */
    ngx_list_t                        headers;

    /*
     * 响应状态码,必须一直有值
     */
    ngx_uint_t                        status;
    ngx_str_t                         status_line;

    ngx_table_elt_t                  *server;
    ngx_table_elt_t                  *date;
    ngx_table_elt_t                  *content_length;
    ngx_table_elt_t                  *content_encoding;
    ngx_table_elt_t                  *location;
    ngx_table_elt_t                  *refresh;
    ngx_table_elt_t                  *last_modified;
    ngx_table_elt_t                  *content_range;
    ngx_table_elt_t                  *accept_ranges;
    ngx_table_elt_t                  *www_authenticate;
    ngx_table_elt_t                  *expires;
    ngx_table_elt_t                  *etag;

    ngx_str_t                        *override_charset;

    /*
     * The default value for this field is -1, which means that the body size is unknown.
     * In this case, chunked transfer encoding is used
     *
     * 响应体的字节个数,如果设置为-1则表示不知道响应体大小,那么后续会使用chunk编码输出内容
     */
    size_t                            content_type_len;
    ngx_str_t                         content_type;
    ngx_str_t                         charset;
    u_char                           *content_type_lowcase;
    ngx_uint_t                        content_type_hash;

    ngx_array_t                       cache_control;

    /*
     * 响应体大小
     */
    off_t                             content_length_n;
    time_t                            date_time;
    time_t                            last_modified_time;
} ngx_http_headers_out_t;


typedef void (*ngx_http_client_body_handler_pt)(ngx_http_request_t *r);

typedef struct {
    ngx_temp_file_t                  *temp_file;
    ngx_chain_t                      *bufs;
    ngx_buf_t                        *buf;
    off_t                             rest;
    ngx_chain_t                      *free;
    ngx_chain_t                      *busy;
    ngx_http_chunked_t               *chunked;
    ngx_http_client_body_handler_pt   post_handler;
} ngx_http_request_body_t;


typedef struct ngx_http_addr_conf_s  ngx_http_addr_conf_t;

typedef struct {
    ngx_http_addr_conf_t             *addr_conf;
    // TODO 当前连接对应的server{}块中的ngx_http_conf_ctx_t结构体
    ngx_http_conf_ctx_t              *conf_ctx;

#if (NGX_HTTP_SSL && defined SSL_CTRL_SET_TLSEXT_HOSTNAME)
    ngx_str_t                        *ssl_servername;
#if (NGX_PCRE)
    ngx_http_regex_t                 *ssl_servername_regex;
#endif
#endif

    /*
     * 用来存放cscf->large_client_header_buffers指令配置的内存块,结构如下
     * 			busy
     * 			-----
     * 			| * |
     *			-----
     *			   \
     *			  --------------------
     *			  | * | * | 共large_client_header_buffers.num个
     *			  --------------------
     */
    ngx_buf_t                       **busy;
    /*
     * 目前已经使用的busy的个数,该值不会大于large_client_header_buffers.num
     */
    ngx_int_t                         nbusy;

    ngx_buf_t                       **free;
    ngx_int_t                         nfree;

#if (NGX_HTTP_SSL)
    unsigned                          ssl:1;
#endif
    unsigned                          proxy_protocol:1;
} ngx_http_connection_t;


typedef void (*ngx_http_cleanup_pt)(void *data);

typedef struct ngx_http_cleanup_s  ngx_http_cleanup_t;

struct ngx_http_cleanup_s {
    ngx_http_cleanup_pt               handler;
    void                             *data;
    ngx_http_cleanup_t               *next;
};


/*
 * 子请求结束时的回调方法? 应该不是结束时,是每次输出数据后吧  TODO
 * r: 子请求
 * data: ngx_http_post_subrequest_t结构体的data字段
 * rc: 子请求的返回值
 *
 */
typedef ngx_int_t (*ngx_http_post_subrequest_pt)(ngx_http_request_t *r,
    void *data, ngx_int_t rc);

typedef struct {
    ngx_http_post_subrequest_pt       handler;
    void                             *data;
} ngx_http_post_subrequest_t;


/*
 * 用来表示某个请求的子请求的结构体
 */
typedef struct ngx_http_postponed_request_s  ngx_http_postponed_request_t;
struct ngx_http_postponed_request_s {
	/*
	 * 父请求产生的子请求,该字段有值表示这是一个子请求节点,和out字段互斥
	 */
    ngx_http_request_t               *request;

    /*
     * 父请求产生的数据,该字段有值表示这是一个数据节点,和reqeust字段互斥
     */
    ngx_chain_t                      *out;

    ngx_http_postponed_request_t     *next;
};


/*
 * 用来表示某个主请求下的所有子请求的结构体
 */
typedef struct ngx_http_posted_request_s  ngx_http_posted_request_t;
struct ngx_http_posted_request_s {
	/*
	 * 主请求(request->main)关联的子请求
	 */
    ngx_http_request_t               *request;
    ngx_http_posted_request_t        *next;
};


/*
 * http模块阶段中要执行的方法,比如
 * 		/src/http/ngx_http_core_module.h/ngx_http_phase_handler_s结构体中的handler字段
 *		/src/http/ngx_http_request.h/ngx_http_request_s结构体中的content_handler字段
 *
 * 返回值代表的意思(基本如此,特殊情况看各自的checker方法,参考用):
 *		NGX_DECLINED: 代表本阶段还未执行完毕
 *		NGX_AGAIN | rc == NGX_DONE: 表示当前方法未执行完毕
 *		NGX_OK: 代表本阶段执行结束,交给脚本引擎去执行下一个阶段(有特殊情况)
 *
 *
 */
typedef ngx_int_t (*ngx_http_handler_pt)(ngx_http_request_t *r);
typedef void (*ngx_http_event_handler_pt)(ngx_http_request_t *r);


struct ngx_http_request_s {
    uint32_t                          signature;         /* "HTTP" */

    /*
     * 指向一个ngx_connection_t客户端连接对象,对个请求可以指向同一个该对象,这些请求必须是
     * 一个主请求和他的所有子请求.
     *
     * 这个对象里面的data字段指向到一个请求,那么这个请求就是活跃的(active),一个活跃的对象用
     * 来处理这个链接上的是事件,并且只有活跃的请求对象才允许向客户端输出数据.所以在一个有子请求
     * 的主请求中,每一个相关的请求都可以变成活跃请求,然后向外输出数据.
     */
    ngx_connection_t                 *connection;

    /*
     * 一个存储http模块上下文的数组,请求中每一个NGX_HTTP_MODULE类型的模块都可以在这个数组中
     * 存放自己的数据(一般是一个指针指向一个结构体),每个模块的存储位置就是每个模块的ctx_index
     * 的值,这个值就是每个模块在ctx数组中的下标.
     *
     * ngx提供了两个便捷的方法来存储并获取这个值
     * 	ngx_http_get_module_ctx(r, module): 获取module模块存放在ctx中的值
     * 	ngx_http_set_ctx(r, c, module): 设置module模块存放在ctx中的值
     */
    void                            **ctx;

    /*
     * 用来存放所有http模块在当前请求的http{}内的配置结构体信息
     */
    void                            **main_conf;

    /*
     * 用来存放所有http模块在当前请求的server{}内的配置结构体信息
     */
    void                            **srv_conf;

    /*
     * 用来存储所有http模块在当前请求的location{}内的配置结构体信息
     *
     * 该字段在匹配location{}的过程会用到,匹配成功就会设置该字段,下面两个方法会执行匹配操作
     * 	/src/http/ngx_http_core_module.c/ngx_http_core_find_location()
     * 	/src/http/ngx_http_core_module.c/ngx_http_core_find_static_location()
     *
     *
     * 方法/src/http/ngx_http_script.c/ngx_http_script_if_code(),会为if(){}块中的loc_conf字段赋值,
     * 前提是if(){}块存在于一个location()块中。注意location()块中的loc_conf和他内部的if(){}块的loc_conf字段不是同一个,
     * if(){}会为自己的loc_conf字段重新分配内存。
     */
    void                            **loc_conf;

    /*
     * 当前请求的读写事件处理方法,通常一个http连接的读写事件都被设置为ngx_http_request_handler()
     * 方法,这个方法根据具体情况来调用当前活跃的请求的读写方法(即下面两个方法).
     */
    ngx_http_event_handler_pt         read_event_handler;
    ngx_http_event_handler_pt         write_event_handler;

#if (NGX_HTTP_CACHE)
    /*
     * Request cache object for caching the upstream response.
     */
    ngx_http_cache_t                 *cache;
#endif

    /*
     * Request upstream object for proxying.
     */
    ngx_http_upstream_t              *upstream;
    ngx_array_t                      *upstream_states;
                                         /* of ngx_http_upstream_state_t */

    ngx_pool_t                       *pool;

    /*
     * 用来存储读取到的http请求头(字符串)的buf
     *
     * 大小有client_header_buffer_size指令设置,默认1k
     *
     * 如果超过则使用large_client_header_buffers指令的设置,默认是4个8k缓存块
     */
    ngx_buf_t                        *header_in;

    /*
     * 解析并结构化的http请求头和响应头
     * 另外这两个字段都包含一个ngx_list_t类型的headers字段,这个里面包含了原生的头列表
     * 除此之外一些常用的头被单独拿出来,以便方便使用,比如status、content_length_n等
     */
    ngx_http_headers_in_t             headers_in;
    ngx_http_headers_out_t            headers_out;

    ngx_http_request_body_t          *request_body;

    time_t                            lingering_time;

    /*
     * 请求被创建的一个时间点,一个是秒,一个是毫秒
     */
    time_t                            start_sec;
    ngx_msec_t                        start_msec;

    /*
     * http请求方法的一个数字描述,在ngx中使用宏来表示,比如
     * NGX_HTTP_GET、NGX_HTTP_HEAD、NGX_HTTP_POST等
     */
    ngx_uint_t                        method;
    /*
     * 用来表示http客户端请求的协议版本号,用数字表示,比如:
     *	NGX_HTTP_VERSION_10
     *	NGX_HTTP_VERSION_11
     */
    ngx_uint_t                        http_version;

    /*
     * http请求行,未做百分号解码,比如请求行:
     * 	 GET /add/%28%28%28 HTTP/1.1
     * 注意这里的请求行不带后面的换行符
     */
    ngx_str_t                         request_line;

    /*
     * http请求行,做过百分号解码,比如对于请求:
     * 	  /add/%28%28%28?a=b
     * 那么该字段值为:
     * 	  /add/(((
     * 不包括查询参数
     *
     * 这个字段的值就不是单纯指向headers_in的指针了,因为它涉及到对原数据的修改,所以会单独拷贝一份
     */
    ngx_str_t                         uri;

    /*
     * 原生请求参数,比如请求:
     * 	 /add?name=%28%28%28
     * 那么该值为:
     * 	 name=%28%28%28
     */
    ngx_str_t                         args;

    /*
     * 做过百分号解码的请求扩展名,比如请求:
     * 	/add.ht%28ml
     * 那么该字段值为:
     * 	ht(ml
     */
    ngx_str_t                         exten;

    /*
     * 未解析的请求uri,比如请求:
     * 	  ///add/%28%28%28.htm%28l?name=%28%28%28
     * 那么该值就是原始请求值,包括查询参数
     */
    ngx_str_t                         unparsed_uri;

    /*
	 * http请求方法的一个字符描述,GET、HEAD、POST等
	 */
    ngx_str_t                         method_name;
    /*
     * 用来表示http客户端请求的协议版本号,用字符串表示,比如:
     * 	HTTP/1.0
     * 	HTTP/1.1
     * 同http_version字段
     */
    ngx_str_t                         http_protocol;

    ngx_chain_t                      *out;

    /*
     * Pointer to a main request object.
     * This object is created to process a client HTTP request, as opposed to subrequests,
     * which are created to perform a specific subtask within the main request.
     *
     * 指向一个http主请求对象,而不是用来完成子任务的子请求对象
     */
    ngx_http_request_t               *main;

    /*
     * Pointer to the parent request of a subrequest.
     *
     * 指向当前子请求的父级对象
     */
    ngx_http_request_t               *parent;

    /*
     * 一个当前请求要输出的数据和当前请求的子请求的列表,别表内部按照数据的发送和子请求的创建顺序排序.
     * 这里列表由postpone过滤器来保证请求的输出顺序
     * 例如:
     * 		----------
     * 		| parent |
     * 		----------
     * 			\
     * 		   --------      --------      --------
     * 		   | sub1 | ---> | data | ---> | sub2 |
     * 		   --------      --------      --------
     *
     */
    ngx_http_postponed_request_t     *postponed;

    /*
     * 指向一个处理程序,当子请求结束的时候调用该处理程序.
     *
     * 实际上至少有两次被调用,第一次是当执行完content_phase阶段checker后通过调用finalize_request方法来触发
     * 这个处理程序;第二次是子请求真正执行完毕后通过调用finalize_request方法来触发
     */
    ngx_http_post_subrequest_t       *post_subrequest;

    /*
     * 一个请求列表,ngx通过调用列表中存放的请求对象的write_event_handler()方法来启动或者恢复这个请求
     *
     * 通常通过ngx_http_post_request(r, NULL)方法把请求放入主请求的这个列表中.
     * 提交到该列表的请求通过ngx_http_run_posted_requests(c)方法触发,通常调用完一个请求的读写事件方法
     * 后就会调用ngx_http_run_posted_requests()方法
     *
     * 对于http请求来说,这个链接上的读写事件方法都是ngx_http_request_handler(),该方法会调用请求的读写
     * 方法read|write_event_handler(),最后再调用ngx_http_run_posted_requests()
     */
    ngx_http_posted_request_t        *posted_requests;

    /*
     * 当前正在执行的handler的索引
     *
     * 这个索引是cmcf->phase_engine.handlers数组的一个下标
     */
    ngx_int_t                         phase_handler;
    ngx_http_handler_pt               content_handler;
    ngx_uint_t                        access_code;

    /*
     * 每个请求都有cmcf->variables.nelts个变量值
     *
     * 其中cmcf->variables中存放的是在ngx配置文件中出现的所有变量的名字(ngx_http_variable_t)
     * 这个字段保证了每个请求的变量都是项目独立的
     */
    ngx_http_variable_value_t        *variables;

#if (NGX_PCRE)
    ngx_uint_t                        ncaptures;
    int                              *captures;
    u_char                           *captures_data;
#endif

    size_t                            limit_rate;
    size_t                            limit_rate_after;

    /* used to learn the Apache compatible response length without a header */
    size_t                            header_size;

    /*
     * 记录真个请求的长度,比如
     * 		GET /add/%28%28%28 HTTP/1.1
     * 它会把协议后面的回车换行符也计算在内
     */
    off_t                             request_length;

    ngx_uint_t                        err_status;

    ngx_http_connection_t            *http_connection;
#if (NGX_HTTP_SPDY)
    ngx_http_spdy_stream_t           *spdy_stream;
#endif

    ngx_http_log_handler_pt           log_handler;

    ngx_http_cleanup_t               *cleanup;

    /*
     * 在调用ngx_http_create_request()方法创建主请求的时候被设置,目前大小是201
     * 代表允许发起的子请求的个数,后续当前主请求试图发起一个子请求前会先把该值减去一,
     * 如果减去一后的值等于零,那么就不会在允许创建子请求,所以ngx中子请求最多200个
     */
    unsigned                          subrequests:8;

    /*
     * 一个请求引用计数器,这个字段仅对主请求有效,一个正常单一请求则该值为1
     *
     * 通过 r->main->count++ 这种方式来增加这个计数器
     * 通过调用ngx_http_finalize_request(r, rc)方法来减小计数器
     *
     * 创建一个子请求和读取请求体都需要增加了这个计数器
     *
     * 主请求刚开始的时候会设置这个值为1,ngx_http_create_request()方法中有如下操作
     *  r->main = r;
     *	r->count = 1;
     *
     * 只有这个计数器为0的时候才能结束这个主请求
     *  一般是进入ngx_http_close_request()方法之前是1,在这个方法中减去1后等于零,此时才是一个
     *  正常的结束过程.
     *
     * ngx设计这个字段的目的是为了能够正确关闭主请求,因为ngx中的大部分操作都是"异步并行"执行的
     * 一个例子:
     * 	假设一个主请求同时发起了三个子请求,在ngx中这四个请求(加上主请求)并不是按照发起的顺序同步
     * 	执行的,当这些请求被发起后他们都各自并行执行,如果这个时候主请求先执行完毕,那么他会调用请求
     * 	结束方法去关闭请求对应的资源,此时资源真的被关闭后会发生什么情况的?其它三个子请求肯定会因为
     * 	找不到对应的资源而不知所错。
     * 另一个例子:
     *  假设一个主请求需要读取请求体,这个读请求体和主请求本身的处理也是一个并行操作,如果请求体非常
     *  大,就有可能在请求体还没有读完的时候主请求已经独立完成了,如果这个时候去释放主请求的资源,那
     *  读请求体的操作就会不知所错了。
     * 为了避免出现上面的情况,ngx设计了一个主请求引用计数器,这个计数器用来记录某个主请求在执行时与
     * 它并行的动作的个数,每个动作再完成后都会主动将计数器减一,主请求也会根据这个技术器的值来确定是
     * 否可以关闭并释放器对应的资源。
     */
    unsigned                          count:8;
    unsigned                          blocked:8;

    unsigned                          aio:1;

    unsigned                          http_state:4;

    /* URI with "/." and on Win32 with "//" */
    unsigned                          complex_uri:1;

    /* URI with "%" */
    unsigned                          quoted_uri:1;

    /* URI with "+" */
    unsigned                          plus_in_uri:1;

    /* URI with " " */
    unsigned                          space_in_uri:1;

    unsigned                          invalid_header:1;

    unsigned                          add_uri_to_alias:1;
    unsigned                          valid_location:1;
    unsigned                          valid_unparsed_uri:1;

    // 标记uri是否改变了
    unsigned                          uri_changed:1;

    /*
     * 在调用ngx_http_create_request()方法创建主请求和调用ngx_http_subrequest()方法
     * 创建子请求的时候被设置,目前大小是11; 后续在每次更改uri或者匹配named类型的location的
     * 时候会先将该值减去1,如果减去一后的值等于零,那么就不会在允许做后续操作,所以ngx中最多10次
     * uri变更操作和匹配named类型的location操作
     */
    unsigned                          uri_changes:4;

    unsigned                          request_body_in_single_buf:1;
    unsigned                          request_body_in_file_only:1;
    unsigned                          request_body_in_persistent_file:1;
    unsigned                          request_body_in_clean_file:1;
    unsigned                          request_body_file_group_access:1;
    unsigned                          request_body_file_log_level:3;

    /* TODO */
    unsigned                          request_body_no_buffering:1;

    // 子请求产生的数据需要在内存中
    unsigned                          subrequest_in_memory:1;
    unsigned                          waited:1;

#if (NGX_HTTP_CACHE)
    unsigned                          cached:1;
#endif

#if (NGX_HTTP_GZIP)
    unsigned                          gzip_tested:1;
    unsigned                          gzip_ok:1;
    unsigned                          gzip_vary:1;
#endif

    unsigned                          proxy:1;
    unsigned                          bypass_cache:1;
    unsigned                          no_cache:1;

    /*
     * instead of using the request context data in
     * ngx_http_limit_conn_module and ngx_http_limit_req_module
     * we use the single bits in the request structure
     */
    unsigned                          limit_conn_set:1;
    unsigned                          limit_req_set:1;

#if 0
    unsigned                          cacheable:1;
#endif

    unsigned                          pipeline:1;
    unsigned                          chunked:1;
    unsigned                          header_only:1;
    unsigned                          keepalive:1;
    unsigned                          lingering_close:1;
    unsigned                          discard_body:1;
    unsigned                          reading_body:1;
    unsigned                          internal:1;
    unsigned                          error_page:1;
    unsigned                          filter_finalize:1;
    unsigned                          post_action:1;
    unsigned                          request_complete:1;
    unsigned                          request_output:1;
    unsigned                          header_sent:1;
    unsigned                          expect_tested:1;
    unsigned                          root_tested:1;
    unsigned                          done:1;
    unsigned                          logged:1;

    unsigned                          buffered:4;

    /*
     * 看/src/http/ngx_http_copy_filter_module.c/ngx_http_copy_filter():ctx->need_in_memory
     * 代表主请求和他的所有子请求的输出数据必须在内存中生产,不能在文件中,对于copy filter来说,这个标记会忽略
     * sendfile功能
     *
     * ? 还没明白main_filter_need_in_memory、filter_need_in_memory、filter_need_temporary这三个字段的具体区别
     */
    unsigned                          main_filter_need_in_memory:1;
    /*
	 * 看/src/http/ngx_http_copy_filter_module.c/ngx_http_copy_filter():ctx->need_in_memory
	 * 和main_filter_need_in_memory标记一样,该标记值标记当前这个请求
	 */
    unsigned                          filter_need_in_memory:1;
    /*
	 * 看/src/http/ngx_http_copy_filter_module.c/ngx_http_copy_filter():ctx->need_in_temp
	 *
	 * Flag requesting that the request output be produced in temporary buffers, but not in
	 * readonly memory buffers or file buffers. This is used by filters which may change
	 * output directly in the buffers where it's sent.
	 */
    unsigned                          filter_need_temporary:1;
    unsigned                          allow_ranges:1;
    unsigned                          single_range:1;
    unsigned                          disable_not_modified:1;

#if (NGX_STAT_STUB)
    unsigned                          stat_reading:1;
    unsigned                          stat_writing:1;
#endif

    /* used to parse HTTP headers */

    /*
     * 下面这八个字段在解析HTTP头的时候用到,比如目前用到这些字段的方法
     *   ngx_http_parse_request_line()解析请求行
     *   ngx_http_parse_status_line()解析响应状态行
     * 	 ngx_http_parse_header_line()解析请求头
     * 用下面的响应数据解析这些字段的意义
     *   HTTP/1.1 200 OK
	 *	 Server: MyNgx
	 *	 Date: Sun, 26 Nov 2017 06:36:44 GMT
	 *	 Content-Type: image/jpeg
	 *	 Content-Length: 7092
	 *	 Connection: keep-alive
     */


    /* 当前解析的状态,上面三个_line()方法呢都有一个state枚举(enum),用来表示当前解析时的状 */
    ngx_uint_t                        state;

    /* 解析到的头hash值,比HTTP头"Server" */
    ngx_uint_t                        header_hash;
    /* 如果解析到的小写形式的HTTP头是"server",那么lowcase_header[lowcase_index]就是这个头最后一个字符的下一个位置 */
    ngx_uint_t                        lowcase_index;
    /* 解析到的小写形式的HTTP头,比如"server" */
    u_char                            lowcase_header[NGX_HTTP_LC_HEADER_LEN];

    /*
     * 解析到的一个HTTP头的开始地址,比如HTTP头"Server"在内存中的开始地址
     * 一个动态值,每开始解析一个请求头时他都会指向当前解析头的开始地址,
     */
    u_char                           *header_name_start;
    /* "Server"这个HTTP头在内存中的结束地址 */
    u_char                           *header_name_end;
    /* HTTP头"Server"的值("MyNgx")的内存开始地址*/
    u_char                           *header_start;
    /* HTTP头"Server"的值("MyNgx")的内存结束地址*/
    u_char                           *header_end;

    /*
     * a memory that can be reused after parsing a request line
     * via ngx_http_ephemeral_t
     */

    /*
     * uri开始地址
     * 	   GET /lua HTTP/1.1
     * 指向/
     */
    u_char                           *uri_start;
    /*
     * uri的结束地址,uri的最后一个字符的后面,比如
     * 		GET /get?name=age
     * 该字段指向最后一个字符e的后面,也就是一个空格' '
     */
    u_char                           *uri_end;
    u_char                           *uri_ext;
    /*
     * 查询参数开始位置,不包括问号
     *
     * uri_end - args_start 就是所有的查询参数
     */
    u_char                           *args_start;
    /*
     * 请求行的开始指针,比如请求是:
     * 		GET /lua HTTP/1.1
     * 那么该指针值就是G这个字符的地址
     */
    u_char                           *request_start;
    /*
     * 请求行的结束指针,比如请求的是:
     * 		GET /lua HTTP/1.1\r\n
     * 那么该指针指向最后一个字符'1'的后面的字符'\r'
     */
    u_char                           *request_end;
    /*
     * 方法的最后一个字母,比如方法是GET,那么该指针指向的就是T这个字符
     */
    u_char                           *method_end;
    u_char                           *schema_start;
    u_char                           *schema_end;
    u_char                           *host_start;
    u_char                           *host_end;
    u_char                           *port_start;
    u_char                           *port_end;

    /*
     * 用数字表示的http协议主次版本号,比如
     *	HTTP/1.1
     * 则http_major=1,http_minor=1
     */
    unsigned                          http_minor:16;
    unsigned                          http_major:16;
};


typedef struct {
    ngx_http_posted_request_t         terminal_posted_request;
} ngx_http_ephemeral_t;


#define ngx_http_ephemeral(r)  (void *) (&r->uri_start)


extern ngx_http_header_t       ngx_http_headers_in[];
extern ngx_http_header_out_t   ngx_http_headers_out[];


#define ngx_http_set_log_request(log, r)                                      \
    ((ngx_http_log_ctx_t *) log->data)->current_request = r


#endif /* _NGX_HTTP_REQUEST_H_INCLUDED_ */
