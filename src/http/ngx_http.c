
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * 如果操作系统支持sendfile方法,那么ngx就使用/src/os/unix/ngx_linux_sendfile_chain()方法
 * 来发送数据,即使sendfile指令为off也会使用这个方法因为这个方法既可以发送(ngx_writev)内存中buf,
 * 也可以发送(ngx_linux_sendfile)文件中buf
 *
 * 如果操作系统不支持sendfile方法,那么ngx就使用/src/os/unix/ngx_writev_chain()方法发送数据
 *
 *
 * http模块中,各个配置信息结构体的关联方式:
 * 	1.在http核心模块的ngx_http_core_main_conf_t里面有一个servers数组,用来关联http{}块下的所有server{}
 *
 *	2.在http核心模块的ngx_http_core_srv_conf_t里面有一个ctx(ngx_http_conf_ctx_t),该结构体的srv_conf字段用来关联
 *    所有http模块在server{}块内的配置信息结构体;
 *    loc_conf字段用来关联所有http模块在location{}块内的指令在上层server{}块内的合并项;
 *    在loc_conf数组的第一个位置放的是ngx_http_core_loc_conf_t结构体,该结构体的locations队列用来关联当前server{}块
 *    下的所有location{}块
 *
 *  3.同理ngx_http_core_loc_conf_t里面的locations队列关联当前locaiton{}块下的所有location{}块
 *
 *
 * 第三方模块如要要介入到http的执行阶段中,需要在ngx_http_module_t.postconfiguration()方法中将自己的handler注册到
 * 对应的阶段数组中.不能在ngx_http_module_t.preconfiguration()方法中注册,因为preconfiguration()方法执行时各个阶段
 * 的数组还没有被初始化。
 *
 *
 * 过滤器有三种:
 * 		ngx_http_output_header_filter_pt ngx_http_top_header_filter: 处理响应头的过滤器
 * 		ngx_http_output_body_filter_pt ngx_http_top_body_filter: 处理响应体的过滤器
 * 		ngx_http_request_body_filter_pt ngx_http_top_request_body_filter: 处理请求体的过滤器,当需要读取请求体时使用
 * 上面这三种变量是一个全局变量,每一个变量代表一种过滤器链,每一个想把自己加入到这个链中的过滤器,都会把自己设置为链头,当然为了避免链的断
 * 裂,在把自己设置为链头之前会把原先的链头保存起来,等自己的过滤器方法执行完毕之后再调用原来的链头,所以ngx这种过滤器组装链的形式很像是一
 * 个压栈的过程.
 *
 * 举个例子:
 *	假设有5个过滤器,注册顺序分别是A、B、C、D、E,组装链的过程我们用一个压栈的方式来解释,当然实际上并没有这个栈,只是用它了辅助解释:
 *	1.把过滤器A压人栈顶
 *		| A |
 *		-----
 *	 此时过滤器A就是链头
 *
 *	2.把过滤器B压人栈顶
 *		| B |
 *		| A |
 *		-----
 *   在压的过程中过滤器B会先把链头保存起来,之后把自己设置为链头,所以此时B即使栈顶有是链头
 *
 *  3.把过滤器C压人栈顶
 *  	| C |
 *  	| B |
 *  	| A |
 *		-----
 *	  这个过程跟过滤器B是一样的,同样C先把链头保存下来,把自己设置为链头,此时C就是新链头了,而C保存的老链头就是B
 *
 *	4.接着继续把D、E压人栈顶,结果如下
 *	  	| E |
 *	  	| D |
 *	  	| C |
 *	  	| B |
 *	  	| A |
 *	  	-----
 *	 这样就形成了一个过滤器链,链头是E, 从栈的结构可以看出,注册时排在最前面的过滤器A处于栈底,也就是过滤链的最尾部,
 *	 而注册顺序排在最后的过滤器E且变成了链头,也就是说注册顺序和执行顺序是相反的。
 *
 * 上面的例子是一个过滤链组装过程的解释,过滤器的执行过程就是一个遍历链表的过程,举例如下:
 * 	1.执行过滤器E本身的过滤方法
 * 	2.调用过滤器E之前存放的链头过滤器D的方法
 *	   --->	| E |
 *			| D |
 *			| C |
 *			| B |
 *			| A |
 *			-----
 *
 * 	3.执行过滤器D本身的过滤方法
 * 	4.调用过滤器D之前存放的链头过滤器C的方法
 * 			| E |
 *	   ---> | D |
 *			| C |
 *			| B |
 *			| A |
 *			-----
 *
 * 	5.依次类推,最后就会指向到过滤器A,因为过滤A是最后一个,所以他不会再调用其它的过滤了,它的功能就是输出数据并结束过滤器链的调用
 *	 		| E |
 *	   		| D |
 *			| C |
 *			| B |
 *	   --->	| A |
 *			-----
 *
 * 举一个实际的例子,对于一个header类型的过滤器,过滤器方法取名为my_header_filter,他需要有一个header类型的过滤器变量,如
 * 		ngx_http_output_header_filter_pt ngx_http_next_header_filter
 * 我们用它来存储老的链头,这里为他取名next是按照执行循序命名的,因为执行的时候老的链头过滤器就是下一个要执行的过滤器.
 * 当前过滤器存储老链头过滤器:
 * 		ngx_http_next_header_filter = ngx_http_top_head_filter
 * 把当前过滤器方法设置为新链头:
 * 		ngx_http_top_head_filter = my_header_filter
 * 如此一来就把自定义的过滤器放入到了过滤器链表中,过滤器执行的时候只需要调用链头方法(ngx_http_top_head_filter)就可以了.
 *
 *
 *
 * 一般使用的处理响应头的过滤器有以下几种,他们的位置是固定的,按倒序执行
 *  	/src/http/ngx_http_header_filter_module.c				(header)
 * 		/src/http/modules/ngx_http_chunked_filter_module.c		(body|header)
 * 		/src/http/modules/ngx_http_range_filter_module.c		(body|header) 	(ngx_http_range_header_filter_module)
 * 		/src/http/modules/ngx_http_gzip_filter_module.c			(body|header)
 * 		/src/http/modules/ngx_http_ssi_filter_module.c			(body|header)
 * 		/src/http/modules/ngx_http_charset_filter_module.c 		(body|header)
 * 		/src/http/modules/ngx_http_userid_filter_module.c		(header)
 * 		/src/http/modules/ngx_http_headers_filter_module.c		(header)
 * 		/src/http/modules/ngx_http_not_modified_filter_module.c (header)
 *
 * 一般使用的处理响应体的过滤器有以下几种,他们的位置是固定的,按倒序执行
 *  	/src/http/ngx_http_write_filter_module.c				(body)
 * 		/src/http/modules/ngx_http_chunked_filter_module.c		(body|header)
 * 		/src/http/modules/ngx_http_gzip_filter_module.c			(body|header)
 * 		/src/http/ngx_http_postpone_filter_module.c				(body)
 * 		/src/http/modules/ngx_http_ssi_filter_module.c			(body|header)
 * 		/src/http/modules/ngx_http_charset_filter_module.c		(body|header)
 *  	/src/http/ngx_http_copy_filter_module.c					(body)
 * 		/src/http/modules/ngx_http_range_filter_module.c		(body|header)   (ngx_http_range_body_filter_module)
 *
 *
 *
 * ------------------------------------结合ngx_http_request.c文件中的注释看--------------------------------
 * 一个主请求的开始: 无限循环执行ngx_process_events_and_timers()方法
 *  1.当请求过来的时候先执行ngx_http_init_connection()方法获取tcp连接
 *
 *  2.然后设置c->read->handler = ngx_http_wait_request_handler
 *
 *  3.ngx_http_wait_request_handler等待正真的请求数据过来,然后调用ngx_http_create_request()
 *    来创建主请求,至此一个请求真正开始,并且c->data为当前请求,此后c->data就一直表示可以向客户端输出数据的请求
 *
 *  5.然后更改rev->->handler = ngx_http_process_request_line
 *    ngx_http_process_request_line()方法处理请求行
 *
 *  6.请求行处理完毕后个更改rev->handler = ngx_http_process_request_headers
 *    ngx_http_process_request_headers()方法解析请求头
 *
 *  7.请求头解析完毕后调用ngx_http_process_request()方法,这个方法开始调用
 *      ngx_http_handler()方法执行ngx_http_core_run_phases()方法
 *      ngx_http_run_posted_requests()方法执行子请求
 *
 *  8.ngx_http_core_run_phases()方法用来执行阶段引擎,它会一次调用各个阶段的checker,最终会调用到
 *    内容checker(ngx_http_core_content_phase)
 *
 *    内容checker调用内容hanlder方法(r->content_handler),然后将返回值传递给(ngx_http_finalize_request)方法:
 *    一般的内容handler方法(r->content_handler)都会通过调用ngx_http_output_filter()来执行过滤器,然后返回
 *			ngx_http_finalize_request(r, r->content_handler(r));
 *			return NGX_OK;
 *	  至此,阶段引擎执行完毕,控制权交给ngx,ngx则继续执行下一个事件
 *
 *	9.如果数据没有别一次输出完毕,则在ngx_http_finalize_request()方法中调用ngx_http_set_write_handler()
 *	  把当前请求的写事件方法设置为ngx_http_writer或者ngx_http_core_run_phases
 *	  		r->write_event_handler = ngx_http_writer;
 *	  		r->write_event_handler = ngx_http_core_run_phases;
 *	  ngx_http_writer()方法会重新调用ngx_http_output_filter()重走过滤器
 *	  ngx_http_core_run_phases()方法则重走阶段引擎,具体执行那个阶段看r->phase_handler的值
 *
 *
 *
 * ------------------------------------------ngx_http_finalize_request()方法调用次数--------------------------------------
 * 对于一个http请求,在其整个生命周中一般回调用两次ngx_http_finalize_request()方法
 *   第一次:当请求的写事件方法还是ngx_http_core_run_phases()方法的时候,在最后一个阶段checker(ngx_http_core_content_phase)中会调用
 *   			ngx_http_finalize_request(r, r->content_handler(r))方法或者
 *   			ngx_http_finalize_request(r, rc)方法来结束请求
 *   	   如果这一调用就可以把数据都输出完毕,那么只调用一次ngx_http_finalize_request()方法就可以了
 *
 *   第二次:当请求的响应数据没办法一次输出完毕的时候,在ngx_http_finalize_request()方法中会调用ngx_http_set_write_handler(r)方法,
 *   	   将当前请求的写事件设置为ngx_http_writer()方法,而ngx_http_writer()方法会重新调用ngx_http_output_filter()方法来重启过滤器
 *   	   来输出数据:
 *   	   如果数据还是没有输出完毕,则直接返回等待一次事件到来后继续启动过滤器输出数据;
 *   	   如果数据输出完毕了则第二次调用ngx_http_finalize_request()方法来结束这个请求;
 *
 *
 *
 *
 *
 *
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static char *ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_init_phases(ngx_conf_t *cf,
    ngx_http_core_main_conf_t *cmcf);
static ngx_int_t ngx_http_init_headers_in_hash(ngx_conf_t *cf,
    ngx_http_core_main_conf_t *cmcf);
static ngx_int_t ngx_http_init_phase_handlers(ngx_conf_t *cf,
    ngx_http_core_main_conf_t *cmcf);

static ngx_int_t ngx_http_add_addresses(ngx_conf_t *cf,
    ngx_http_core_srv_conf_t *cscf, ngx_http_conf_port_t *port,
    ngx_http_listen_opt_t *lsopt);
static ngx_int_t ngx_http_add_address(ngx_conf_t *cf,
    ngx_http_core_srv_conf_t *cscf, ngx_http_conf_port_t *port,
    ngx_http_listen_opt_t *lsopt);
static ngx_int_t ngx_http_add_server(ngx_conf_t *cf,
    ngx_http_core_srv_conf_t *cscf, ngx_http_conf_addr_t *addr);

static char *ngx_http_merge_servers(ngx_conf_t *cf,
    ngx_http_core_main_conf_t *cmcf, ngx_http_module_t *module,
    ngx_uint_t ctx_index);
static char *ngx_http_merge_locations(ngx_conf_t *cf,
    ngx_queue_t *locations, void **loc_conf, ngx_http_module_t *module,
    ngx_uint_t ctx_index);
static ngx_int_t ngx_http_init_locations(ngx_conf_t *cf,
    ngx_http_core_srv_conf_t *cscf, ngx_http_core_loc_conf_t *pclcf);
static ngx_int_t ngx_http_init_static_location_trees(ngx_conf_t *cf,
    ngx_http_core_loc_conf_t *pclcf);
static ngx_int_t ngx_http_cmp_locations(const ngx_queue_t *one,
    const ngx_queue_t *two);
static ngx_int_t ngx_http_join_exact_locations(ngx_conf_t *cf,
    ngx_queue_t *locations);
static void ngx_http_create_locations_list(ngx_queue_t *locations,
    ngx_queue_t *q);
static ngx_http_location_tree_node_t *
    ngx_http_create_locations_tree(ngx_conf_t *cf, ngx_queue_t *locations,
    size_t prefix);

static ngx_int_t ngx_http_optimize_servers(ngx_conf_t *cf,
    ngx_http_core_main_conf_t *cmcf, ngx_array_t *ports);
static ngx_int_t ngx_http_server_names(ngx_conf_t *cf,
    ngx_http_core_main_conf_t *cmcf, ngx_http_conf_addr_t *addr);
static ngx_int_t ngx_http_cmp_conf_addrs(const void *one, const void *two);
static int ngx_libc_cdecl ngx_http_cmp_dns_wildcards(const void *one,
    const void *two);

static ngx_int_t ngx_http_init_listening(ngx_conf_t *cf,
    ngx_http_conf_port_t *port);
static ngx_listening_t *ngx_http_add_listening(ngx_conf_t *cf,
    ngx_http_conf_addr_t *addr);
static ngx_int_t ngx_http_add_addrs(ngx_conf_t *cf, ngx_http_port_t *hport,
    ngx_http_conf_addr_t *addr);
#if (NGX_HAVE_INET6)
static ngx_int_t ngx_http_add_addrs6(ngx_conf_t *cf, ngx_http_port_t *hport,
    ngx_http_conf_addr_t *addr);
#endif

ngx_uint_t   ngx_http_max_module;


/*
 * 处理响应头的过滤器,目前有以下模块:
 * 		/src/http/ngx_http_header_filter_module.c				(header)
 * 		/src/http/modules/ngx_http_chunked_filter_module.c		(body|header)
 * 		/src/http/modules/ngx_http_range_filter_module.c		(body|header)
 * 		/src/http/modules/ngx_http_gzip_filter_module.c			(body|header)
 * 		/src/http/modules/ngx_http_ssi_filter_module.c			(body|header)
 * 		/src/http/modules/ngx_http_charset_filter_module.c 		(body|header)
 * 		/src/http/modules/ngx_http_userid_filter_module.c		(header)
 * 		/src/http/modules/ngx_http_headers_filter_module.c		(header)
 * 		/src/http/modules/ngx_http_not_modified_filter_module.c (header)
 *
 *		/src/http/modules/ngx_http_addition_filter_module.c 	(body|header)
 *		/src/http/modules/ngx_http_gunzip_filter_module.c		(body|header)
 *		/src/http/modules/ngx_http_image_filter_module.c		(body|header)
 *		/src/http/modules/ngx_http_sub_filter_module.c			(body|header)
 *		/src/http/modules/ngx_http_xslt_filter_module.c			(body|header)
 *		/src/http/ngx_http_spdy_filter_module.c
 *
 *	其中ngx_http_header_filter_module是header类型的最后一个过滤器,只有这个过滤器才会真正
 *	发送数据,并且还是调用的ngx_http_write_filter_module过滤器中的方法,中间的其它过滤器基本
 *	都是在设置响应头
 *
 */
ngx_http_output_header_filter_pt  ngx_http_top_header_filter;

/*
 * 处理响应体的过滤器,目前有以下模块:
 *  	/src/http/ngx_http_write_filter_module.c				(body)
 * 		/src/http/modules/ngx_http_chunked_filter_module.c		(body|header)
 * 		/src/http/modules/ngx_http_gzip_filter_module.c			(body|header)
 * 		/src/http/ngx_http_postpone_filter_module.c				(body)
 * 		/src/http/modules/ngx_http_ssi_filter_module.c			(body|header)
 * 		/src/http/modules/ngx_http_charset_filter_module.c		(body|header)
 *  	/src/http/ngx_http_copy_filter_module.c					(body)
 * 		/src/http/modules/ngx_http_range_filter_module.c		(body|header)
 *
 *		/src/http/modules/ngx_http_addition_filter_module.c		(body|header)
 *		/src/http/modules/ngx_http_gunzip_filter_module.c		(body|header)
 *		/src/http/modules/ngx_http_image_filter_module.c		(body|header)
 *		/src/http/modules/ngx_http_sub_filter_module.c			(body|header)
 *		/src/http/modules/ngx_http_xslt_filter_module.c			(body|header)
 *
 * 其中ngx_http_write_filter_module是body类型的最后一个过滤器
 *
 */
ngx_http_output_body_filter_pt    ngx_http_top_body_filter;
/*
 * 处理请求体的过滤器,目前有以下模块:
 *		/src/http/ngx_http_core_module.c  (ngx_http_top_request_body_filter = ngx_http_request_body_save_filter)
 */
ngx_http_request_body_filter_pt   ngx_http_top_request_body_filter;


ngx_str_t  ngx_http_html_default_types[] = {
    ngx_string("text/html"),
    ngx_null_string
};


static ngx_command_t  ngx_http_commands[] = {

    { ngx_string("http"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_http_block,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_http_module_ctx = {
    ngx_string("http"),
    NULL,
    NULL
};


ngx_module_t  ngx_http_module = {
    NGX_MODULE_V1,
    &ngx_http_module_ctx,                  /* module context */
    ngx_http_commands,                     /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/*
 * 在解析ngx顶层配置文件时会解析到http{}指令,该指令对应该方法
 *
 * 指令http对应的方法,整个http模块的入口,用来管理所有的http模块
 *
 * 调用该方法之前conf_ctx的内存分配到了第二层
 *
 *
 * cf的值最初是由ngx_init_cycle()方法调用ngx_conf_parse()方法传过来的,cf内容如下：
 * 	 cf->ctx: cycle->conf_ctx
 *	 cf->module_type = NGX_CORE_MODULE
 *	 cf->cmd_type = NGX_MAIN_CONF
 *
 * ngx_conf_parse()方法在调用完ngx_conf_read_token()解析出一个指令"http"后调用
 * /src/core/ngx_conf_file.c/ngx_conf_handler()方法,这个方法会会做一些判断和设置,
 * 然后在回调命令中的方法(cmd->set(cf, cmd, conf)).
 *
 * ngx_conf_handler()方法遍历所有的模块指令,从中查找匹配的指令,其中一个比较就是
 * 当前要查找的指令区域(cf->cmd_type)是否和指令(http)应该在的区域(cmd->type)相同,
 * 这样可以区分出相同指令名,但所在区域不同的指令(也就是说不同模块之间可以有相同的指令名,前提是所在区域不同)。
 *
 *
 * cmd: 该指令的配置信息
 * 	    ngx_string("http"),
 *      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
 *
 *
 * conf:
 *   因为http指令有NGX_MAIN_CONF配置,所以conf值走的是ngx_conf_handler()方法的
 * 		} else if (cmd->type & NGX_MAIN_CONF) {
 * 			conf = &(((void **) cf->ctx)[ngx_modules[i]->index]);
 *   逻辑。
 *
 *   该值是核心http模块在cycle->conf_ctx中第二层指针的位置的地址:
 *   (此时cycle->conf_ctx等于cf->ctx)
 *
 *   cf->ctx      	     conf
 *   -----          	-----
 *	 | * |              | * |
 *	 -----              -----
 *	  \                 /
 *	   ------------------
 *	   | * | * | * | *# |ngx_http_module.index
 *     ------------------
 *
 * conf值是 &cycle->conf_ctx[ngx_http_module.index]
 *
 */
static char *
ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                        *rv;
    ngx_uint_t                   mi, m, s;
    ngx_conf_t                   pcf;
    ngx_http_module_t           *module;
    ngx_http_conf_ctx_t         *ctx;
    ngx_http_core_loc_conf_t    *clcf;
    ngx_http_core_srv_conf_t   **cscfp;
    ngx_http_core_main_conf_t   *cmcf;

    /*
     * 此时*ctx的内存结构如下:(*号代表指针类型;#号代表空;*#代表指针类型并且是空)
     * 	   ctx
     * 	  ------
     * 	  | *# |
     * 	  ------
     *
     *
     * 如果conf存在,则表示已经为核心http模块(ngx_http_module)创建过存储配置信息的结构体,
     * 不应该再次进入到该指令方法,也就是说不应该存在一个以上的http指令
     */
    if (*(ngx_http_conf_ctx_t **) conf) {
        return "is duplicate";
    }


    /* the main http context */
    /*
     * 分配完内存后ctx的结构如下
     *   ctx
     *  -----
     *  | * |
     *  -----
     *   \
     *   -----------------------
     *   | ngx_http_conf_ctx_t |
     *   -----------------------
     */
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    // 为cycle->conf_ctx[ngx_http_module.index]=ctx
    /*
     * 这里这样赋值,则只用到了cycle->conf_ctx中的两层指针,如下图:
     * (其中cf->ctx 等于 cycle->conf_ctx)
     *
     *   cf->ctx             conf
	 *   -----              -----
	 *	 | * |              | * |
	 *	 -----              -----
	 *	  \                 /      				   ctx
	 *	  -----------------       				  -----
	 *	  | * |  ...  | * |       				  | * |
	 *    -----------------       				  -----
	 *    				  \      				  /
	 *    				   -----------------------
     *				       | ngx_http_conf_ctx_t |
     *				       -----------------------
     *
     */
    *(ngx_http_conf_ctx_t **) conf = ctx;


    /* count the number of the http modules and set up their indices */
    /* 计算http模块个数,并且为所有http模块设置索引值 */

    ngx_http_max_module = 0;
    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {
            continue;
        }

        // 计算http模块个数并设置索引值
        ngx_modules[m]->ctx_index = ngx_http_max_module++;
    }


    /* the http main_conf context, it is the same in the all http contexts */

    /*
     *  ctx->main_conf用于存放所有http模块在http{}块下的配置信息结构体,内存分配完毕后结构如下:
     *  cf->ctx | cycle->conf_ctx
	 *   -----
	 *	 | * |
	 *	 -----
	 *	  \            ngx_http_module.index					 ctx
	 *	   -----------------									-----
	 *	   | * |  ...  | * |									| * |
	 *     -----------------									-----
	 *    				 \         ngx_http_conf_ctx_t			 /
	 *    			     -----------------------------------------
	 *    			     | **main_conf | **srv_conf | **loc_conf |
	 *			         -----------------------------------------
	 *			             \
	 *			             ---------------
	 *			             | * | ... | * |  ngx_http_max_module个
     *						 ---------------
     *
     */
    ctx->main_conf = ngx_pcalloc(cf->pool,
                                 sizeof(void *) * ngx_http_max_module);
    if (ctx->main_conf == NULL) {
        return NGX_CONF_ERROR;
    }


    /*
     * the http null srv_conf context, it is used to merge
     * the server{}s' srv_conf's
     */

    /*
     * ctx->srv_conf用于存放所有http模块在server{}级指令的合并信息。
     *
	 * 在http{}块下可以有多个server{}块,对于同一个指令,在不同的server{}块内可以有不同的值,很显然一个
	 * ctx->srv_conf是无法存放多个server{}块内指令信息的,那ctx->srv_conf作用究竟是什么呢?我们假设
	 * 有一个指令,它在多个server{}块内的值都是相同的,此时我们在所有server{}快内把该指令都设置一遍就产生
	 * 了很多的冗余,解决这个问题的办法就是把该指令设置在server{}的上一层,即http{}块内,这样在解析各个
	 * server{}块时就可以从http{}内继承该指令,从而解决指令配置冗余的问题。
	 *
	 * 分配完毕后内存结构如下:
	 *   cf->ctx | cycle->conf_ctx
	 *   -----
	 *	 | * |
	 *	 -----
	 *	  \            ngx_http_module.index					 ctx
	 *	   -----------------									-----
	 *	   | * |  ...  | * |									| * |
	 *     -----------------									-----
	 *    				 \         ngx_http_conf_ctx_t			 /
	 *    			     -----------------------------------------
	 *    			     | **main_conf | **srv_conf | **loc_conf |
	 *			         -----------------------------------------
	 *			             				\
	 *			             			---------------
	 *			             			| * | ... | * |  ngx_http_max_module个
     *						 			---------------
     */
    ctx->srv_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);
    if (ctx->srv_conf == NULL) {
        return NGX_CONF_ERROR;
    }


    /*
     * the http null loc_conf context, it is used to merge
     * the server{}s' loc_conf's
     */

    /*
     * ctx->loc_conf用于存放所有http模块在location{}级指令的合并信息。
     *
     * 同server{}块一样,location{}块在server{}块内同样可以有多个,所以ctx->loc_conf的作用
     * 同ctx->srv_conf一样,可以解决某个http模块在location{}块内的指令冗余问题。
	 *
	 * 分配完毕后内存结构如下:
	 *   cf->ctx | cycle->conf_ctx
	 *   -----
	 *	 | * |
	 *	 -----
	 *	  \            ngx_http_module.index					 ctx
	 *	   -----------------									-----
	 *	   | * |  ...  | * |									| * |
	 *     -----------------									-----
	 *    				 \         ngx_http_conf_ctx_t			 /
	 *    			     -----------------------------------------
	 *    			     | **main_conf | **srv_conf | **loc_conf |
	 *			         -----------------------------------------
	 *			             							\
	 *			             						---------------
	 *			             						| * | ... | * |  ngx_http_max_module个
     *						 						---------------
     */
    ctx->loc_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);
    if (ctx->loc_conf == NULL) {
        return NGX_CONF_ERROR;
    }


    /*
     * create the main_conf's, the null srv_conf's, and the null loc_conf's
     * of the all http modules
     */

    /*
     * 回调所有HTTP模块的 create_main|srv|loc_conf 方法
     * 这时候只是在http{}块为每个http模块创建了各自的自定义配置信息结构体
     *
     * 每个模块在server{}块的自定义配置项还没有创建,在后续调用的ngx_conf_parse()方法中如果解析到server指令,
     * 就会先回调server指令的ngx_http_core_module.c/ngx_http_core_server()回调方法来处理server指令,
     * ngx_http_core_server()会调用所有HTTP模块create_srv|loc_conf方法,为每个模块在server{}块创建各自的
     * 自定义配置信息结构体。
     *
     * 下面这个循环完毕之后,所有http模块的create_main_conf|create_srv_conf|create_loc_conf方法都会被调用,
     * 也就是说在http{}块内,每个http模块都有可能被创建三个配置信息结构体,来存放他们的指令信息。
     *
     */
    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = ngx_modules[m]->ctx;
        mi = ngx_modules[m]->ctx_index;

        /*
         * 创建完毕后,内存结构如下:
         * 	 cf->ctx | cycle->conf_ctx
         *   -----
         *	 | * |
         *	 -----
         *	  \            ngx_http_module.index					  ctx
         *	   -----------------									 -----
         *	   | * |  ...  | * | ngx_max_module个					 | * |
         *     -----------------									 -----
         *    				 \         ngx_http_conf_ctx_t			 /
         *    			     -----------------------------------------
         *    			     | **main_conf | **srv_conf | **loc_conf |
         *			         -----------------------------------------
         *			           \				\				\
         *			            ---------------
         *			            | * | ... | * | ngx_http_max_module个
         *			            ---------------
         *			              \
         *						  ---------------------------
         *						  |ngx_http_core_main_conf_t|
         *						  ---------------------------
         */
        if (module->create_main_conf) {
            ctx->main_conf[mi] = module->create_main_conf(cf);
            if (ctx->main_conf[mi] == NULL) {
                return NGX_CONF_ERROR;
            }
        }


        /*
		 * 创建完毕后,内存结构如下:
		 * 	 cf->ctx | cycle->conf_ctx
		 *   -----
		 *	 | * |
		 *	 -----
		 *	  \     ngx_http_module.index					  ctx
		 *	   ---------									 -----
		 *	   | * | * | ngx_max_module个					 | * |
		 *     ---------									 -----
		 *    		 \         ngx_http_conf_ctx_t			 /
		 *    		 -----------------------------------------
		 *    		 | **main_conf | **srv_conf | **loc_conf |
		 *			 -----------------------------------------
		 *			   		\		 \					\
		 *			    		 	 ---------------
		 *			    		 	 | * | ... | * | ngx_http_max_module个
		 *			    		 	 ---------------
		 *			              	   \
		 *						  	   --------------------------
		 *						  	   |ngx_http_core_srv_conf_t|
		 *						  	   --------------------------
		 */
        if (module->create_srv_conf) {
            ctx->srv_conf[mi] = module->create_srv_conf(cf);
            if (ctx->srv_conf[mi] == NULL) {
                return NGX_CONF_ERROR;
            }
        }


        /*
		 * 创建完毕后,内存结构如下:
		 * 	 cf->ctx | cycle->conf_ctx
		 *   -----
		 *	 | * |
		 *	 -----
		 *	  \     ngx_http_module.index					 ctx
		 *	   ---------									-----
		 *	   | * | * | ngx_max_module个					| * |
		 *     ---------									-----
		 *    		 \         ngx_http_conf_ctx_t			/
		 *    		 -----------------------------------------
		 *    		 | **main_conf | **srv_conf | **loc_conf |
		 *			 -----------------------------------------
		 *			   		\		 	\			\
		 *			    		 	 				---------------
		 *			    		 	 				| * | ... | * | ngx_http_max_module个
		 *			    		 	 				---------------
		 *			              	  				  \
		 *						  	  				  --------------------------
		 *						  	   				  |ngx_http_core_loc_conf_t|
		 *						  	   				  --------------------------
		 */
        if (module->create_loc_conf) {
            ctx->loc_conf[mi] = module->create_loc_conf(cf);
            if (ctx->loc_conf[mi] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }


    /*
     * 下面这两句执行完毕后 pcf和cf的区别仅仅是 ->ctx的值不一样
     * cycle->conf_ctx
	 *   -----
	 *	 | * |
	 *	 -----
	 *	  \     ngx_http_module.index				 ctx|cf->ctx
	 *	   ---------									-----
	 *	   | * | * | ngx_max_module个					| * |
	 *     ---------									-----
	 *    		 \         ngx_http_conf_ctx_t			/
	 *    		 -----------------------------------------
	 *    		 | **main_conf | **srv_conf | **loc_conf |
	 *			 -----------------------------------------
	 *			   		\		 	\				\
	 *			    		 	 					---------------
	 *			    		 	 					| * | ... | * | ngx_http_max_module个
	 *			    		 	 					---------------
	 *			              	  				  	  \
	 *						  	  				  	 --------------------------
	 *						  	   				  	 |ngx_http_core_loc_conf_t|
	 *						  	   				  	 --------------------------
     */
    pcf = *cf;
    cf->ctx = ctx;


    /*
     * 执行所有http模块的module->preconfiguration()方法
     */
    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = ngx_modules[m]->ctx;

        // 如果方法存在则回调该方法
        if (module->preconfiguration) {
            if (module->preconfiguration(cf) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
        }
    }

    /* parse inside the http{} block */

    // 解析http模块
    cf->module_type = NGX_HTTP_MODULE;
    // 解析http{}块内的指令
    cf->cmd_type = NGX_HTTP_MAIN_CONF;

    /*
     * 解析http{}内的所有指令
     * 每解析到一个指令就会调用该指令的handler方法
     * 每个指令的handler方法一般负责为该指令赋值
     * 指令的handler方法可以是自定义的,也可以使用nginx提供的预设方法
     *
     * 当该方法执行完后,所有的http{}块内的指令都被执行完了. (包块server{}s,location{}s,算是一个递归执行)
     * 遇见server{}指令就会执行/src/http/ngx_http_core_server()方法
     */
    rv = ngx_conf_parse(cf, NULL);

    if (rv != NGX_CONF_OK) {
        goto failed;
    }

    /*
     * init http{} main_conf's, merge the server{}s' srv_conf's
     * and its location{}s' loc_conf's
     */

    /*
     * cycle->conf_ctx
	 *   -----
	 *	 | * |
	 *	 -----
	 *	  \     ngx_http_module.index				 ctx|cf->ctx
	 *	   ---------									-----
	 *	   | * | * | ngx_max_module个					| * |
	 *     ---------									-----
	 *    		 \         ngx_http_conf_ctx_t			/
	 *    		 -----------------------------------------
	 *    		 | **main_conf | **srv_conf | **loc_conf |
	 *			 -----------------------------------------
	 *			   		\		 	   \		\     cmcf
	 *			      ---------------				  -----
	 *			      | * | ... | * |				  | * |
	 *			      ---------------				  -----
	 *			         \ ngx_http_core_main_conf_t  /
	 *					 ------------------------------
	 *					 | servers |		...  	  |
	 *					 ------------------------------
     */
    cmcf = ctx->main_conf[ngx_http_core_module.ctx_index];
    // 取出http{}块下的所有server
    cscfp = cmcf->servers.elts;

    // 初始化和merge所有http模块在http{},server{}s,location{}s块的配置项
    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = ngx_modules[m]->ctx;
        mi = ngx_modules[m]->ctx_index;

        /* init http{} main_conf's */
        // 调用该模块main级别的初始化方法,因为它不涉及跟上层信息合并,所以叫init
        if (module->init_main_conf) {
            rv = module->init_main_conf(cf, ctx->main_conf[mi]);
            if (rv != NGX_CONF_OK) {
                goto failed;
            }
        }

        // 调用该http模块在server级的merge_srv_conf方法和merge_loc_conf方法来合并上层(http{})的指令配置信息
        rv = ngx_http_merge_servers(cf, cmcf, module, mi);
        if (rv != NGX_CONF_OK) {
            goto failed;
        }
    }



    /* create location trees */
    /*
     * 开始设置location查找树
     */

    for (s = 0; s < cmcf->servers.nelts; s++) {

    	// clcf->locations队列中存放了当前server下的所有location{}
        clcf = cscfp[s]->ctx->loc_conf[ngx_http_core_module.ctx_index];

        /*
         * cscfp[s]: 当前server{}块的ngx_http_core_srv_conf_t结构体
         * clcf: 当前server{}块的ngx_http_core_loc_conf_t结构体
         *
         * 初始化locations队列,该方法执行完毕后会把locations队列拆分开来
         */
        if (ngx_http_init_locations(cf, cscfp[s], clcf) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        /*
         * 此时locations队列已经由ngx_http_init_locations()方法分隔完毕
         * 组装完全二叉树
         */
        if (ngx_http_init_static_location_trees(cf, clcf) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    /*
     * 初始化存放http各个阶段方法的数组
     * 这一步完成之后第三方模块才能把自己的方法注册到对应的阶段中
     */
    if (ngx_http_init_phases(cf, cmcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    // 初始化存放http请求头的hash结构
    if (ngx_http_init_headers_in_hash(cf, cmcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    /*
     * 到这里http模块的基本配置就算完成了,下面这个循环会回调所有http模块的postconfiguration()方法
     * 所以基本上在postconfiguration()方法中可以拿到所有的配置信息
     *
     * 基本上,所有模块都是在这个方法中把自己注册到某个阶段的,所以,模块向某个阶段容器的注册顺序跟模块的编译顺序是一样的
     *
     * 但并不是所有模块都会实现postconfiguration()方法,比如ngx_http_geo_module.c,它只是在解析到geo指令后向变量
     * 容器中增加了某个变量而已
     */
    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = ngx_modules[m]->ctx;

        if (module->postconfiguration) {
            if (module->postconfiguration(cf) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
        }
    }

    /*
     * 到这里所有的指令都已经解析完毕,包括set指令
     *
     * 该函数的作用是为cmcf->variables中的变量设置get_handler方法(这些方法都是内置变量或者动态变量)
     */
    if (ngx_http_variables_init_vars(cf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    /*
     * http{}'s cf->ctx was needed while the configuration merging
     * and in postconfiguration process
     */

    *cf = pcf;


    // 将注册的http中注册的handler(cmcf->phases[i].handlers)放到cmcf->phase_engine.handlers阶段引擎中
    if (ngx_http_init_phase_handlers(cf, cmcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }


    /* optimize the lists of ports, addresses and server names */

    /*
     * 这个方法是http模块将自己的方法注册到事件模块的入口
	 *
     *  这个方法会调用ngx_http_init_listening方法，去初始化从server{}中解析到的监听地址和端口
     *  ngx_http_init_listening方法实际上去调用ngx_http_add_listening方法创建ngx_listening_t结构体

     *  然后ngx_http_add_listening方法会把创建好的ngx_listening_t结构体，通过ngx_create_listening方法放入到
     *  ngx_cycle_t->listening数组中。
     *  接下来再设置ngx_listening_t->handler = ngx_http_init_connection
     *  (nginx事件框架使用ngx_event_accept来获取新连接,并在获取完新连接后回调ngx_listening_t->handler方法)
     *  (ngx_cycle_t->listening数组中所代表的连接被放入到epoll之前,他们的读事件会通过事件模块的ngx_event_process_init
     *  方法设置为c->read->handler = ngx_event_accept)

     *  初始化连接时(ngx_http_init_connection方法)会设置
     * 	 c->read->handler = ngx_http_wait_request_handler;
     *	 c->write->handler = ngx_http_empty_handler;

     *  一旦有请求过来后ngx_http_wait_request_handler方法会首先创建ngx_http_request_t结构体,然后设置回调函数
	 *	 c->data = ngx_http_create_request(c);
	 *	 rev->handler = ngx_http_process_request_line;
     *	 ngx_http_process_request_line(rev);
     *
     */

    /*
     * 1.通过ports构造域名的hash结构
     * 2.将posts中所有的监听地址,放入cf->cycle->listening数组中
     */
    if (ngx_http_optimize_servers(cf, cmcf, cmcf->ports) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    // 这个方法结束后 ngx_listening_t 中的fd仍然没有被初始化
    return NGX_CONF_OK;

failed:

    *cf = pcf;

    return rv;
}


/*
 * 初始化http模块的阶段,其实就是初始化每个阶段的数组,只有初始化完毕后第三方模块才能将自己的方法注入到不同的阶段
 *
 * 只初试化了7个阶段,其它四个阶段是不可介入的
 */
static ngx_int_t
ngx_http_init_phases(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf)
{
    if (ngx_array_init(&cmcf->phases[NGX_HTTP_POST_READ_PHASE].handlers,
                       cf->pool, 1, sizeof(ngx_http_handler_pt))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&cmcf->phases[NGX_HTTP_SERVER_REWRITE_PHASE].handlers,
                       cf->pool, 1, sizeof(ngx_http_handler_pt))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers,
                       cf->pool, 1, sizeof(ngx_http_handler_pt))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers,
                       cf->pool, 1, sizeof(ngx_http_handler_pt))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers,
                       cf->pool, 2, sizeof(ngx_http_handler_pt))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers,
                       cf->pool, 4, sizeof(ngx_http_handler_pt))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers,
                       cf->pool, 1, sizeof(ngx_http_handler_pt))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


/*
 * 初始化请求头的hash结构 cmcf->headers_in_hash
 *
 * headers_in_hash结构中key是请求头的名字,值是ngx_http_header_t结构体,该结构体则存放了
 * 获取当前请求头值的方法
 */
static ngx_int_t
ngx_http_init_headers_in_hash(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf)
{
    ngx_array_t         headers_in;
    ngx_hash_key_t     *hk;
    ngx_hash_init_t     hash;
    ngx_http_header_t  *header;

    if (ngx_array_init(&headers_in, cf->temp_pool, 32, sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    for (header = ngx_http_headers_in; header->name.len; header++) {
        hk = ngx_array_push(&headers_in);
        if (hk == NULL) {
            return NGX_ERROR;
        }

        hk->key = header->name;
        // 忽略大小写的hash值
        hk->key_hash = ngx_hash_key_lc(header->name.data, header->name.len);
        hk->value = header;
    }

    hash.hash = &cmcf->headers_in_hash;
    hash.key = ngx_hash_key_lc;
    hash.max_size = 512;
    hash.bucket_size = ngx_align(64, ngx_cacheline_size);
    hash.name = "headers_in_hash";
    hash.pool = cf->pool;
    hash.temp_pool = NULL;

    if (ngx_hash_init(&hash, headers_in.elts, headers_in.nelts) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


/*
 * 从该方法的代码逻辑可知
 * 	NGX_HTTP_FIND_CONFIG_PHASE
 *	NGX_HTTP_POST_REWRITE_PHASE
 *	NGX_HTTP_POST_ACCESS_PHASE
 *	NGX_HTTP_TRY_FILES_PHASE
 * 以上四个阶段的执行handler方法是固定的,第三方模块无法接入
 *
 * 该方法的主要作用是把注册到cmcf->phases[i].handlers中的方法,放到cmcf->phase_engine.handlers阶段引擎中。
 * ngx本身的模块或者是第三方模块,如果要介入到请求操作中,就需要把方法注册到每个阶段代表的handler数组中,而cmcf->phases[i].handlers
 * 就是那个数组。
 *
 */
static ngx_int_t
ngx_http_init_phase_handlers(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf)
{
    ngx_int_t                   j;
    ngx_uint_t                  i, n;
    ngx_uint_t                  find_config_index, use_rewrite, use_access;
    ngx_http_handler_pt        *h;
    ngx_http_phase_handler_t   *ph;
    ngx_http_phase_handler_pt   checker;

    cmcf->phase_engine.server_rewrite_index = (ngx_uint_t) -1;
    cmcf->phase_engine.location_rewrite_index = (ngx_uint_t) -1;

    // NGX_HTTP_FIND_CONFIG_PHASE阶段在脚本引擎cmcf->phase_engine.handlers中的开始索引
    find_config_index = 0;

    // 如果NGX_HTTP_REWRITE_PHASE阶段注册了方法,则说明使用了rewrite阶段
    use_rewrite = cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers.nelts ? 1 : 0;
    // 确定是否在NGX_HTTP_ACCESS_PHASE阶段注册了方法
    use_access = cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers.nelts ? 1 : 0;

    /*
     * 因为有下面四个阶段
     *  	NGX_HTTP_FIND_CONFIG_PHASE
     *		NGX_HTTP_POST_REWRITE_PHASE
     *		NGX_HTTP_POST_ACCESS_PHASE
     *		NGX_HTTP_TRY_FILES_PHASE
     * 是不允许被其它模块介入的,但是他们也是在脚本引擎中被执行的,所以需要在脚本引擎的handlers中(cmcf->phase_engine.handlers)
     * 分配一个ngx_http_phase_handler_t来执行对应的checker,从下面的循环代码可以看到,计算脚本引擎的
     * handlers中ngx_http_phase_handler_t的个数用的是cmcf->phases[i].handlers.nelts,也就是每个阶段注册方法个数的总和,
     * 这里并没有包含上面四个阶段,因为四个阶段是不允许被介入的,所以他们对应的注册方法个数也就是0,下面的这个表达式就是把他们加上。
     *
     * use_rewrite: 代表使用了NGX_HTTP_REWRITE_PHASE阶段,那么后续就需要NGX_HTTP_POST_REWRITE_PHASE阶段来做rewrite操作
     * use_access: 代表使用了NGX_HTTP_ACCESS_PHASE阶段,那么后续就需要NGX_HTTP_POST_ACCESS_PHASE阶段在做访问控制操作
     * cmcf->try_files: 使用了NGX_HTTP_TRY_FILES_PHASE阶段
     * 1: NGX_HTTP_FIND_CONFIG_PHASE阶段是必须使用的,所以固定写1
     */
    n = use_rewrite + use_access + cmcf->try_files + 1 /* find config phase */;

    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {
    	/* 没有包括NGX_HTTP_LOG_PHASE阶段 */

    	/*
    	 * 把cmcf->phases中所有注册的方法个数都加起来,如果在对应的不可介入的阶段也注册了方法,那么这些方法的个数也会加起来,
    	 * 这里主要是为了计算需要分配多少个ngx_http_phase_handler_t结构体,后续会根据这个个数来分配内存,但并不一定所有的
    	 * 内存都会被用到,如果在不可介入的阶段也注册了方法,那么有几个多余的方法就会产生几个多余的内存。
    	 */
        n += cmcf->phases[i].handlers.nelts;
    }

    /*
     * 为每一个方法(所有阶段中注册的)分配一个ngx_http_phase_handler_t结构体
     * 阶段引擎会用到这个结构体来执行相应阶段的方法
     *
     * 如果use_rewrite、use_access、cmcf->try_files这三个都有值,那么最终n的值会比阶段中注册的所有方法个数大4,
     * 从实际分配的空间来看,会比实际多出四个ngx_http_phase_handler_t和一个指针的空间大小。
     *
     * 正常情况下cmcf->phase_engine.handlers数组中放的应该是ngx_http_phase_handler_t结构体空间的倍数,但是
     * 这里最后且存放了一个指针空间,目的其实很简单,在cheker方法(ngx_http_core_content_phase)中判断是否是最后
     * 一个ph,而判断的依据是ph->checker是否存在,我们知道ph->checker正好是一个方法指针,所以这里最后多出的指针空间
     * 可以当做ngx_http_phase_handler_t结构体的checker字段占用的空间。
     *
     * 如果最后不用一个指针空间,而是用一个完整的ngx_http_phase_handler_t结构体,这样该结构体中的另两个字段的空间
     * 其实是浪费的,因为ngx判断ph是否是最后一个用的是checker字段,而不是另外两个。
     */
    ph = ngx_pcalloc(cf->pool,
                     n * sizeof(ngx_http_phase_handler_t) + sizeof(void *));
    if (ph == NULL) {
        return NGX_ERROR;
    }

    // 把分配好的ngx_http_phase_handler_t内存空间赋值给阶段引擎的handlers字段
    cmcf->phase_engine.handlers = ph;
    n = 0;

    /*
     * 该循环的目的是为阶段引擎中的handlers(ngx_http_phase_handler_t)设置checker方法和阶段真正要执行的方法
     * ph是一个数组(cmcf->phase_engine.handlers),他包含了所有阶段中的方法,他本身区别阶段的方法就是checker
     * 字段,该字段确定了当前执行的是哪个阶段(在执行的时候有点类似变量的脚本引擎)
     */
    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {
    	// 阶段i中真正执行的方法,也就是用户注册的方法
        h = cmcf->phases[i].handlers.elts;

        switch (i) {

        case NGX_HTTP_SERVER_REWRITE_PHASE:
            if (cmcf->phase_engine.server_rewrite_index == (ngx_uint_t) -1) {
            	// NGX_HTTP_SERVER_REWRITE_PHASE阶段在阶段引擎中的开始索引
                cmcf->phase_engine.server_rewrite_index = n;
            }
            checker = ngx_http_core_rewrite_phase;

            break;

            // 查找location阶段,不可介入
        case NGX_HTTP_FIND_CONFIG_PHASE:
        	// 此时n就是该阶段在脚本引擎cmcf->phase_engine.handlers中的第一个ph
            find_config_index = n;

            ph->checker = ngx_http_core_find_config_phase;

            // 该阶段只需要一个ph,并且没有对应的ph->handler,所以该阶段对应的下一个阶段的开始偏移量加一就可以
            n++;
            // 下一个ph
            ph++;

            /*
             *  ph->checker = checker;
             *	ph->handler = h[j];
             *	ph->next = n;
             *	ph++;
             *
             *	这里直接跳过为ph->handler赋值的逻辑,表示该阶段的handler方法是固定的
             */
            continue;

        case NGX_HTTP_REWRITE_PHASE:
            if (cmcf->phase_engine.location_rewrite_index == (ngx_uint_t) -1) {
            	// NGX_HTTP_REWRITE_PHASE阶段在阶段引擎中的开始索引
                cmcf->phase_engine.location_rewrite_index = n;
            }
            checker = ngx_http_core_rewrite_phase;

            break;

        case NGX_HTTP_POST_REWRITE_PHASE:
            if (use_rewrite) {
            	// 使用了NGX_HTTP_REWRITE_PHASE阶段,所以NGX_HTTP_POST_REWRITE_PHASE阶段就需要占用一个ph
                ph->checker = ngx_http_core_post_rewrite_phase;
                /*
                 * 执行到NGX_HTTP_POST_REWRITE_PHASE阶段后,如果需要重新匹配location,则ph->next就是
                 * NGX_HTTP_FIND_CONFIG_PHASE阶段在脚本引擎handlers数组中的开始索引
                 *
                 * r->uri_changed等于1表示需要重新匹配location
                 */
                ph->next = find_config_index;
                n++;
                ph++;
            }

            /*
			 *  ph->checker = checker;
			 *	ph->handler = h[j];
			 *	ph->next = n;
			 *	ph++;
			 *
			 *	这里直接跳过为ph->handler赋值的逻辑,表示该阶段的handler方法是固定的
             */
            continue;

        case NGX_HTTP_ACCESS_PHASE:
            checker = ngx_http_core_access_phase;
            // 为NGX_HTTP_POST_ACCESS_PHASE阶段预留一个ph
            n++;
            break;

            // 不可介入
        case NGX_HTTP_POST_ACCESS_PHASE:
            if (use_access) {
            	// 使用了NGX_HTTP_ACCESS_PHASE阶段,所以就会用到NGX_HTTP_POST_ACCESS_PHASE阶段
                ph->checker = ngx_http_core_post_access_phase;
                // 这里的n已经在上面的case中为NGX_HTTP_POST_ACCESS_PHASE阶段预留了一个ph,所以n就是该阶段的下一个阶段偏移量
                ph->next = n;
                ph++;
            }

            /*
			 *  ph->checker = checker;
			 *	ph->handler = h[j];
			 *	ph->next = n;
			 *	ph++;
			 *
			 *	这里直接跳过为ph->handler赋值的逻辑,表示该阶段的handler方法是固定的
			 */
            continue;

            // 不可介入
        case NGX_HTTP_TRY_FILES_PHASE:
            if (cmcf->try_files) {
            	// 使用到了NGX_HTTP_TRY_FILES_PHASE阶段
                ph->checker = ngx_http_core_try_files_phase;
                // 该阶段只需要一个ph,并且没有对应的ph->handler,所以该阶段对应的下一个阶段的开始偏移量加一就可以
                n++;
                ph++;
            }

            /*
			 *  ph->checker = checker;
			 *	ph->handler = h[j];
			 *	ph->next = n;
			 *	ph++;
			 *
			 *	这里直接跳过为ph->handler赋值的逻辑,表示该阶段的handler方法是固定的
			 */
            continue;

        case NGX_HTTP_CONTENT_PHASE:
            checker = ngx_http_core_content_phase;
            break;

        default:
        	/*
        	 * NGX_HTTP_POST_READ_PHASE
        	 * NGX_HTTP_PREACCESS_PHASE
        	 *
        	 * 以上三个阶段的checker方法固定为ngx_http_core_generic_phase
        	 */
            checker = ngx_http_core_generic_phase;
        }

        // n在这里表示下一个阶段的开始方法坐标
        n += cmcf->phases[i].handlers.nelts;

        for (j = cmcf->phases[i].handlers.nelts - 1; j >=0; j--) {
        	// 为阶段i的ph设置checker方法
            ph->checker = checker;
            // 为阶段i的ph设置handler方法
            ph->handler = h[j];
            // 阶段i的下一个阶段的开始偏移量
            ph->next = n;
            // 阶段i的下一个ph
            ph++;

            /*
             * 同一个阶段,后注册的模块先执行(模块的注册顺序同编译顺序,在ngx_modules[]中)
             *
             */
        }
    }

    return NGX_OK;
}


/*
 * merge上层(http{})的指令配置信息
 *
 * cf->ctx和cmcf的值如下图:
 *
 *   cycle->conf_ctx
 *   -----
 *	 | * |
 *	 -----
 *	  \     ngx_http_module.index				   cf->ctx
 *	   ---------									-----
 *	   | * | * | ngx_max_module个					| * |
 *     ---------									-----
 *    		 \         ngx_http_conf_ctx_t			/
 *    		 -----------------------------------------
 *    		 | **main_conf | **srv_conf | **loc_conf |
 *			 -----------------------------------------
 *			   		\		 	   \		\     cmcf
 *			      ---------------				  -----
 *			      | * | ... | * |				  | * |
 *			      ---------------				  -----
 *			         \ ngx_http_core_main_conf_t  /
 *					 ------------------------------
 *					 | servers |		...  	  |
 *					 ------------------------------
 *
 * module: 当前http模块的回调方法
 * ctx_index: 当前http模块的数组下标
 *
 */
static char *
ngx_http_merge_servers(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf,
    ngx_http_module_t *module, ngx_uint_t ctx_index)
{
    char                        *rv;
    ngx_uint_t                   s;
    ngx_http_conf_ctx_t         *ctx, saved;
    ngx_http_core_loc_conf_t    *clcf;
    ngx_http_core_srv_conf_t   **cscfp;

    // 把一个连续内存的首地址赋值给一个数组(两层指针),然后就可以使用数组的方式访问了,挺方便
    cscfp = cmcf->servers.elts;
    ctx = (ngx_http_conf_ctx_t *) cf->ctx;
    // 拷贝一份ctx(**main_conf、**srv_conf、**loc_conf的值都会被拷贝走)
    saved = *ctx;
    rv = NGX_CONF_OK;

    for (s = 0; s < cmcf->servers.nelts; s++) {

        /* merge the server{}s' srv_conf's */

    	/*
    	 * 将cf->ctx->srv_conf设置为当前server{}块的srv_conf,不再表示上级(http{})的srv_conf了
    	 * 原始srv_conf是http{}块中的srv_conf,是为了合并http{}块下的server级别的指令信息的
    	 * 操作完毕后最后还会用saved的变量将原始ctx还回去
    	 */
        ctx->srv_conf = cscfp[s]->ctx->srv_conf;

        // 调用该http模块的merge_srv_conf方法,用它来merge上级(http{})区域的配置信息
        if (module->merge_srv_conf) {
        	/*
        	 * cf->ctx: 此时ctx中的srv_conf也就变成当前的srv_conf了,不再表示上级(http{})的srv_conf了
        	 * saved.srv_conf[ctx_index]: 原始配置信息结构体;更容易理解的说法是,该模块在上级(http{})区域的配置信息结构体
        	 * cscfp[s]->ctx->srv_conf[ctx_index]: 该模块在当前区域(server{})的配置信息结构体
        	 */
            rv = module->merge_srv_conf(cf, saved.srv_conf[ctx_index],
                                        cscfp[s]->ctx->srv_conf[ctx_index]);
            if (rv != NGX_CONF_OK) {
                goto failed;
            }
        }

        // 调用该http模块的merge_loc_conf方法,用它来merge上级(http{})区域的配置信息
        if (module->merge_loc_conf) {

            /* merge the server{}'s loc_conf */

        	/*
        	 * 将cf->ctx->loc_conf设置为当前server{]块的loc_conf,不再表示上级(http{})的loc_conf了
        	 */
            ctx->loc_conf = cscfp[s]->ctx->loc_conf;

            /*
             * cf->ctx: 时ctx中的loc_conf就变成当前的loc_conf了,不再表示上级(http{})的loc_conf了
             * saved.loc_conf[ctx_index]: 该模块在上级(http{})区域的配置信息结构体
             * cscfp[s]->ctx->loc_conf[ctx_index]: 该模块在当前区域(server{})的配置信息结构体
             */
            rv = module->merge_loc_conf(cf, saved.loc_conf[ctx_index],
                                        cscfp[s]->ctx->loc_conf[ctx_index]);
            if (rv != NGX_CONF_OK) {
                goto failed;
            }

            /* merge the locations{}' loc_conf's */

            // 取出当前server{}块下的所有location{}
            clcf = cscfp[s]->ctx->loc_conf[ngx_http_core_module.ctx_index];

            /*
             * 去调用该http模块在所有location{}s块的merge_loc_conf方法
             *
             * clcf->locations: 当前server{}块下的所有location{}块
             * cscfp[s]->ctx->loc_conf: 当前server{}块下的loc_conf字段
             * module: 当前http模块的所有回调方法
             * ctx_index: 当前http模块所在的数组下标
             */
            rv = ngx_http_merge_locations(cf, clcf->locations,
                                          cscfp[s]->ctx->loc_conf,
                                          module, ctx_index);
            if (rv != NGX_CONF_OK) {
                goto failed;
            }
        }
    }

failed:

	// 复原ctx字段
    *ctx = saved;

    return rv;
}


/*
 * locations: 当前区域(server{}、location{})下的所有location{}块
 * loc_conf: 上级模块的loc_conf字段
 * module: 当前http模块的回调方法
 * ctx_index: 当前http模块的数组下标
 */
static char *
ngx_http_merge_locations(ngx_conf_t *cf, ngx_queue_t *locations,
    void **loc_conf, ngx_http_module_t *module, ngx_uint_t ctx_index)
{
    char                       *rv;
    ngx_queue_t                *q;
    ngx_http_conf_ctx_t        *ctx, saved;
    ngx_http_core_loc_conf_t   *clcf;
    ngx_http_location_queue_t  *lq;

    if (locations == NULL) {
    	// 没有location{}块则直接返回,递归终结条件
        return NGX_CONF_OK;
    }

    ctx = (ngx_http_conf_ctx_t *) cf->ctx;
    saved = *ctx;

    for (q = ngx_queue_head(locations);
         q != ngx_queue_sentinel(locations);
         q = ngx_queue_next(q))
    {
        lq = (ngx_http_location_queue_t *) q;

        clcf = lq->exact ? lq->exact : lq->inclusive;
        // 设置为当前块的loc_conf字段
        ctx->loc_conf = clcf->loc_conf;

        /*
         * 调用该http模块在location{}块的merge_loc_conf方法
         * loc_conf[ctx_index]: 上级模块配置信息结构体
         * clcf->loc_conf[ctx_index]: 当前模块的配置信息结构体
         */
        rv = module->merge_loc_conf(cf, loc_conf[ctx_index],
                                    clcf->loc_conf[ctx_index]);
        if (rv != NGX_CONF_OK) {
            return rv;
        }

        // 如果location{}块有嵌套,那么递归调用
        rv = ngx_http_merge_locations(cf, clcf->locations, clcf->loc_conf,
                                      module, ctx_index);
        if (rv != NGX_CONF_OK) {
            return rv;
        }
    }

    // 复原ctx字段
    *ctx = saved;

    return NGX_CONF_OK;
}


/*
 * 该方法执行完毕后会把locations队列拆分开来,结果如下:
 *	1.named_locations数组存放@类型的location
 *	2.regex_locations数组存放正则类型的location
 *	3.原来的locations存放无修饰符(包括 =|^~|无修饰符)类型的locaiton
 *	4.if () {} 直接扔掉了,因为他不需要匹配
 */
static ngx_int_t
ngx_http_init_locations(ngx_conf_t *cf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *pclcf)
{
    ngx_uint_t                   n;
    ngx_queue_t                 *q, *locations, *named, tail;
    ngx_http_core_loc_conf_t    *clcf;
    ngx_http_location_queue_t   *lq;
    ngx_http_core_loc_conf_t   **clcfp;
#if (NGX_PCRE)
    ngx_uint_t                   r;
    ngx_queue_t                 *regex;
#endif

    locations = pclcf->locations;

    if (locations == NULL) {
        return NGX_OK;
    }

    // 排序
    ngx_queue_sort(locations, ngx_http_cmp_locations);

    named = NULL;
    n = 0;
#if (NGX_PCRE)
    regex = NULL;
    r = 0;
#endif

    /*
     * 该循环记录locaitons队列中,第一个正则location用regex表示
     * 记录第一个@ location用named表示
     * 记录第一个if () {}用q表示
     */
    for (q = ngx_queue_head(locations);
         q != ngx_queue_sentinel(locations);
         q = ngx_queue_next(q))
    {
        lq = (ngx_http_location_queue_t *) q;

        clcf = lq->exact ? lq->exact : lq->inclusive;

        // 打印排好序的locations
        printf("-------location------->%s %d  %s\n",clcf->name.data,clcf->noname,clcf->root.data);

        if (ngx_http_init_locations(cf, NULL, clcf) != NGX_OK) {
            return NGX_ERROR;
        }

#if (NGX_PCRE)

        // 这是一个正则(~ | ~*)locaiton
        if (clcf->regex) {
            r++;

            if (regex == NULL) {
                regex = q;
            }

            continue;
        }

#endif

        // 这是一个@ location
        if (clcf->named) {
            n++; // 记录named的个数

            if (named == NULL) {
                // 记录第一个@location在队列中的位置
                named = q;
            }

            continue;
        }

        // 这是一个if () {} or limit_except
        if (clcf->noname) {
            break;
        }
    }


    // 把 if () {} 分离出去
    if (q != ngx_queue_sentinel(locations)) {
        ngx_queue_split(locations, q, &tail);
    }

    if (named) {
        clcfp = ngx_palloc(cf->pool,
                           (n + 1) * sizeof(ngx_http_core_loc_conf_t *));
        if (clcfp == NULL) {
            return NGX_ERROR;
        }

        // 存放@匹配的location
        cscf->named_locations = clcfp;

        for (q = named;
             q != ngx_queue_sentinel(locations);
             q = ngx_queue_next(q))
        {
            lq = (ngx_http_location_queue_t *) q;

            *(clcfp++) = lq->exact;
        }

        *clcfp = NULL;

        // 把@匹配的location分离出去
        ngx_queue_split(locations, named, &tail);
    }

#if (NGX_PCRE)

    if (regex) {

        clcfp = ngx_palloc(cf->pool,
                           (r + 1) * sizeof(ngx_http_core_loc_conf_t *));
        if (clcfp == NULL) {
            return NGX_ERROR;
        }

        // 存放正则匹配的location
        pclcf->regex_locations = clcfp;

        for (q = regex;
             q != ngx_queue_sentinel(locations);
             q = ngx_queue_next(q))
        {
            lq = (ngx_http_location_queue_t *) q;

            *(clcfp++) = lq->exact;
        }

        *clcfp = NULL;

        // 把正则匹配的location分离出去
        ngx_queue_split(locations, regex, &tail);
    }

#endif

    return NGX_OK;
}


/*
 * 该方法主要是完成对pclcf->static_locations完全二叉树的赋值
 * static_locations最终值包含精确匹配(=)和无符号匹配的location块
 *
 * 完全二叉树(Complete Binary Tree):
 * 		若设二叉树的深度为h,除第h层外,其它各层的结点数都到达最大个数,并且第h层的节点都连续
 * 		集中在最左边。
 *
 * 		完全二叉树:							 满二叉树
 * 						A						A
 * 					   / \					   / \
 * 					  /   \					  /   \
 * 					 B     C				 B     C
 * 				    / \   / \				/ \   / \
 * 				   D   E F				   D   E F   G
 *
 */
static ngx_int_t
ngx_http_init_static_location_trees(ngx_conf_t *cf,
    ngx_http_core_loc_conf_t *pclcf)
{
    ngx_queue_t                *q, *locations;
    ngx_http_core_loc_conf_t   *clcf;
    ngx_http_location_queue_t  *lq;

    /*
     * 取出pclcf下的locations队列,此时的队列并没有包含全部的location{},只包括  =|^~|无修饰符 这三种类型的locaiton
     * 该队列在/src/http/ngx_http.c/ngx_http_init_locations()方法中已经被分隔完毕
     */
    locations = pclcf->locations;

    if (locations == NULL) {
        return NGX_OK;
    }

    if (ngx_queue_empty(locations)) {
        return NGX_OK;
    }

    for (q = ngx_queue_head(locations);
         q != ngx_queue_sentinel(locations);
         q = ngx_queue_next(q))
    {
        lq = (ngx_http_location_queue_t *) q;

        // 取出代表location{}的ngx_http_core_loc_conf_t结构体
        clcf = lq->exact ? lq->exact : lq->inclusive;

        // 递归处理ngx_http_core_loc_conf_t下的locations队列
        if (ngx_http_init_static_location_trees(cf, clcf) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    // 没看明白是干啥的 TODO ?
    if (ngx_http_join_exact_locations(cf, locations) != NGX_OK) {
        return NGX_ERROR;
    }

    // TODO
    ngx_http_create_locations_list(locations, ngx_queue_head(locations));

    // 创建完全二叉树
    pclcf->static_locations = ngx_http_create_locations_tree(cf, locations, 0);
    if (pclcf->static_locations == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


/*
 * 向locations队列中添加location{}块(ngx_http_core_loc_conf_t)
 *
 * 如果还没有为队列分配内存,则先分配内存
 */
ngx_int_t
ngx_http_add_location(ngx_conf_t *cf, ngx_queue_t **locations,
    ngx_http_core_loc_conf_t *clcf)
{
    ngx_http_location_queue_t  *lq;

    // 检查是否已经为队列分配内存,如果没有则在这里分配
    if (*locations == NULL) {
        *locations = ngx_palloc(cf->temp_pool,
                                sizeof(ngx_http_location_queue_t));
        if (*locations == NULL) {
            return NGX_ERROR;
        }

        // 初始化队列
        ngx_queue_init(*locations);
    }

    // 创建一个队列元素,队列中放的就是该元素
    lq = ngx_palloc(cf->temp_pool, sizeof(ngx_http_location_queue_t));
    if (lq == NULL) {
        return NGX_ERROR;
    }

    if (clcf->exact_match
#if (NGX_PCRE)
        || clcf->regex
#endif
        || clcf->named || clcf->noname)
    {
    	/*
		 * 1.如果该location是精确匹配(=)
		 * 2.如果该location是正则匹配(~|~*)
		 * 3.如果该location是@匹配
		 * 4.nonname: if () {} 或limit_except
		 */
        lq->exact = clcf;
        lq->inclusive = NULL;
        //printf("=====exact====>%s\n",lq->exact->name.data);


    } else {
        /*
	     * 不满足上面的匹配规则,如:
	     * 	location /abc {}
	     *	location ^~ /abc {}
	     */
        lq->exact = NULL;
        lq->inclusive = clcf;
        //printf("=====inclusive====>%s\n",lq->inclusive->name.data);
    }

    // location名字
    lq->name = &clcf->name;
    // location所在的文件
    lq->file_name = cf->conf_file->file.name.data;
    // location所在的行数
    lq->line = cf->conf_file->line;

    //printf("=====lq->file_name====>%s\n",cf->conf_file->file.name.data);
    //printf("=====lq->line====>%lu\n",cf->conf_file->line);

    ngx_queue_init(&lq->list);

    // 将对列元素放到locations队列尾部
    ngx_queue_insert_tail(*locations, &lq->queue);

    return NGX_OK;
}


/*
 * 每个server{}块和location{}块都会维护一个location队列
 * locations队列排完序后的顺序如下:
 *  1. = (精确匹配,终止匹配) | 无修饰符 | ^~ (和无修饰符相同,但终止匹配)
 *    按字符升序排序,如果相同则精确匹配(=)的排在前面
 *    无修饰符和^~修饰符的,不允许出现相同的location
 *  2. ~* | ~
 *    按照在配置文件中的位置排序,终止匹配
 *  3. @ (内部匹配,只匹配一次)
 *  4. if () {} 所有终止匹配都不会终止if
 *
 * 所以基本上ngx把locaiton分成了四种,在这四种locaiton中又分成了三个匹配入口,其中:
 *   1、2是一个入口,先匹配1中的locaiton,不管有没有匹配到都要继续匹配(^~模式除外)2中
 *      的locaiton,如果2中的location没有匹配到则回溯到1中的匹配
 *   1、2是一个入口,先匹配2中的location,如果没匹配到再匹配1中的location
 *   3是一个入口,是一个内部匹配,外部用户看不到
 *   4是一个入口,在rewrite模块中用脚本变量引擎触发的,和变量的运行方式一致
 */
static ngx_int_t
ngx_http_cmp_locations(const ngx_queue_t *one, const ngx_queue_t *two)
{
    ngx_int_t                   rc;
    ngx_http_core_loc_conf_t   *first, *second;
    ngx_http_location_queue_t  *lq1, *lq2;

    lq1 = (ngx_http_location_queue_t *) one;
    lq2 = (ngx_http_location_queue_t *) two;

    first = lq1->exact ? lq1->exact : lq1->inclusive;
    second = lq2->exact ? lq2->exact : lq2->inclusive;

    /*
     * 如果第一个是没有名字的(if () {})
     * 第二个是有名字的(非if () {})
     * 则没有名字的排在后面
     */
    if (first->noname && !second->noname) {
        /* shift no named locations to the end */
        return 1;
    }

    /*
     * 如果第一个是有名字的(非if () {})
     * 第二个是没有名字的(if () {})
     * 则有名字的排在前面
     * 则没有名字的排在后面
     */
    if (!first->noname && second->noname) {
        /* shift no named locations to the end */
        return -1;
    }

    /*
     * 如果都没有名字,则不做排序
     * 比如都是 if () {}就不排序了
     *
     * 从上面到这里的比较都是为了把没有名字的location放到最后
     */
    if (first->noname || second->noname) {
        /* do not sort no named locations */
        return 0;
    }

    /*
     * 第一个是以@开头的location: location @abc {}
     * 第二个不是以@开头的location
     *
     * 则以@开头的排在最后
     */
    if (first->named && !second->named) {
        /* shift named locations to the end */
        return 1;
    }

    // 同上
    if (!first->named && second->named) {
        /* shift named locations to the end */
        return -1;
    }

    // 都以@开头则名字长的排在后面
    if (first->named && second->named) {
        return ngx_strcmp(first->name.data, second->name.data);
    }

#if (NGX_PCRE)

    /*
     * 有正则的排在后面
     */
    if (first->regex && !second->regex) {
        /* shift the regex matches to the end */
        return 1;
    }

    if (!first->regex && second->regex) {
        /* shift the regex matches to the end */
        return -1;
    }

    if (first->regex || second->regex) {
        /* do not sort the regex matches */
        return 0;
    }

#endif

    /**
     * 非正则、非named、非nonamed的location的比较
     * 也就是 无修饰 | ^~ | = 剩下这三种的比较
     * 按字符升序排列,如果排序相同则精确匹配(=)排在前面
     */
    rc = ngx_filename_cmp(first->name.data, second->name.data,
                          ngx_min(first->name.len, second->name.len) + 1);

    if (rc == 0 && !first->exact_match && second->exact_match) {
        /* an exact match must be before the same inclusive one */
    	/*
    	 * 这句保证对于相同名字的location,带等号的(精确匹配)排在其它两种前面
    	 */
        return 1;
    }

    return rc;
}


/**
 * TODO 没看明白目的是啥
 */
static ngx_int_t
ngx_http_join_exact_locations(ngx_conf_t *cf, ngx_queue_t *locations)
{
    ngx_queue_t                *q, *x;
    ngx_http_location_queue_t  *lq, *lx;

    q = ngx_queue_head(locations);

    while (q != ngx_queue_last(locations)) {

        x = ngx_queue_next(q);

        lq = (ngx_http_location_queue_t *) q;
        lx = (ngx_http_location_queue_t *) x;

        if (lq->name->len == lx->name->len
            && ngx_filename_cmp(lq->name->data, lx->name->data, lx->name->len)
               == 0)
        {
        	// 如果locaiton名字相同,并且是精确匹配(=)或者一般匹配(^~|无修饰符),则不允许
            if ((lq->exact && lx->exact) || (lq->inclusive && lx->inclusive)) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                              "duplicate location \"%V\" in %s:%ui",
                              lx->name, lx->file_name, lx->line);

                return NGX_ERROR;
            }

            /*
             * 走到这里是这种情况:
             *  lq->exact == 1  	lx->inclusive == 1
             *  lq->inclusive == 1  	lx->exact == 1
             */
            lq->inclusive = lx->inclusive;

            ngx_queue_remove(x);

            continue;
        }

        q = ngx_queue_next(q);
    }

    return NGX_OK;
}


/*
 * TODO 没仔细看,后续有需要的时候再细看
 */
static void
ngx_http_create_locations_list(ngx_queue_t *locations, ngx_queue_t *q)
{
    u_char                     *name;
    size_t                      len;
    ngx_queue_t                *x, tail;
    ngx_http_location_queue_t  *lq, *lx;

    if (q == ngx_queue_last(locations)) {
        return;
    }

    lq = (ngx_http_location_queue_t *) q;

    if (lq->inclusive == NULL) {
        ngx_http_create_locations_list(locations, ngx_queue_next(q));
        return;
    }

    len = lq->name->len;
    name = lq->name->data;

    for (x = ngx_queue_next(q);
         x != ngx_queue_sentinel(locations);
         x = ngx_queue_next(x))
    {
        lx = (ngx_http_location_queue_t *) x;

        if (len > lx->name->len
            || ngx_filename_cmp(name, lx->name->data, len) != 0)
        {
            break;
        }
    }

    q = ngx_queue_next(q);

    if (q == x) {
        ngx_http_create_locations_list(locations, x);
        return;
    }

    ngx_queue_split(locations, q, &tail);
    ngx_queue_add(&lq->list, &tail);

    if (x == ngx_queue_sentinel(locations)) {
        ngx_http_create_locations_list(&lq->list, ngx_queue_head(&lq->list));
        return;
    }

    ngx_queue_split(&lq->list, x, &tail);
    ngx_queue_add(locations, &tail);

    ngx_http_create_locations_list(&lq->list, ngx_queue_head(&lq->list));

    ngx_http_create_locations_list(locations, x);
}


/*
 * to keep cache locality for left leaf nodes, allocate nodes in following
 * order: node, left subtree, right subtree, inclusive subtree
 *
 * 创建完全二叉树
 *
 * 完全二叉树(Complete Binary Tree):
 *    若设二叉树的深度为h,除第h层外,其它各层的结点数都到达最大个数,并且第h层的节点都连续集中在最左边。
 *
 * 	        完全二叉树:					满二叉树:
 * 	                    A						A
 * 					   / \					   / \
 * 					  /   \					  /   \
 * 					 B     C				 B     C
 * 				    / \   / \				/ \   / \
 * 				   D   E F				   D   E F   G
 */

static ngx_http_location_tree_node_t *
ngx_http_create_locations_tree(ngx_conf_t *cf, ngx_queue_t *locations,
    size_t prefix)
{
    size_t                          len;
    ngx_queue_t                    *q, tail;
    ngx_http_location_queue_t      *lq;
    ngx_http_location_tree_node_t  *node;

    q = ngx_queue_middle(locations);

    lq = (ngx_http_location_queue_t *) q;
    len = lq->name->len - prefix;

    node = ngx_palloc(cf->pool,
                      offsetof(ngx_http_location_tree_node_t, name) + len);
    if (node == NULL) {
        return NULL;
    }

    node->left = NULL;
    node->right = NULL;
    node->tree = NULL;
    node->exact = lq->exact;
    node->inclusive = lq->inclusive;

    node->auto_redirect = (u_char) ((lq->exact && lq->exact->auto_redirect)
                           || (lq->inclusive && lq->inclusive->auto_redirect));

    node->len = (u_char) len;
    ngx_memcpy(node->name, &lq->name->data[prefix], len);

    ngx_queue_split(locations, q, &tail);

    if (ngx_queue_empty(locations)) {
        /*
         * ngx_queue_split() insures that if left part is empty,
         * then right one is empty too
         */
        goto inclusive;
    }

    node->left = ngx_http_create_locations_tree(cf, locations, prefix);
    if (node->left == NULL) {
        return NULL;
    }

    ngx_queue_remove(q);

    if (ngx_queue_empty(&tail)) {
        goto inclusive;
    }

    node->right = ngx_http_create_locations_tree(cf, &tail, prefix);
    if (node->right == NULL) {
        return NULL;
    }

inclusive:

    if (ngx_queue_empty(&lq->list)) {
        return node;
    }

    // 这里放嵌套的location ?
    node->tree = ngx_http_create_locations_tree(cf, &lq->list, prefix + len);
    if (node->tree == NULL) {
        return NULL;
    }

    return node;
}


ngx_int_t
ngx_http_add_listen(ngx_conf_t *cf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_listen_opt_t *lsopt)
{
    in_port_t                   p;
    ngx_uint_t                  i;
    struct sockaddr            *sa;
    struct sockaddr_in         *sin;
    ngx_http_conf_port_t       *port;
    ngx_http_core_main_conf_t  *cmcf;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6        *sin6;
#endif

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    if (cmcf->ports == NULL) {
        cmcf->ports = ngx_array_create(cf->temp_pool, 2,
                                       sizeof(ngx_http_conf_port_t));
        if (cmcf->ports == NULL) {
            return NGX_ERROR;
        }
    }

    sa = &lsopt->u.sockaddr;

    switch (sa->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        sin6 = &lsopt->u.sockaddr_in6;
        p = sin6->sin6_port;
        break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:
        p = 0;
        break;
#endif

    default: /* AF_INET */
        sin = &lsopt->u.sockaddr_in;
        p = sin->sin_port;
        break;
    }

    port = cmcf->ports->elts;
    for (i = 0; i < cmcf->ports->nelts; i++) {
    	// 匹配是否有相同的端口存在于ports中

        if (p != port[i].port || sa->sa_family != port[i].family) {
            continue;
        }

        /* a port is already in the port list */

        /*
         * 端口已经存在,则将地址添加到该端口(&port[i])中
         */
        return ngx_http_add_addresses(cf, cscf, &port[i], lsopt);
    }

    /* add a port to the port list */

    port = ngx_array_push(cmcf->ports);
    if (port == NULL) {
        return NGX_ERROR;
    }

    port->family = sa->sa_family;
    port->port = p;
    port->addrs.elts = NULL;

    return ngx_http_add_address(cf, cscf, port, lsopt);
}


static ngx_int_t
ngx_http_add_addresses(ngx_conf_t *cf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_conf_port_t *port, ngx_http_listen_opt_t *lsopt)
{
    u_char                *p;
    size_t                 len, off;
    ngx_uint_t             i, default_server, proxy_protocol;
    struct sockaddr       *sa;
    ngx_http_conf_addr_t  *addr;
#if (NGX_HAVE_UNIX_DOMAIN)
    struct sockaddr_un    *saun;
#endif
#if (NGX_HTTP_SSL)
    ngx_uint_t             ssl;
#endif
#if (NGX_HTTP_SPDY)
    ngx_uint_t             spdy;
#endif

    /*
     * we cannot compare whole sockaddr struct's as kernel
     * may fill some fields in inherited sockaddr struct's
     */

    sa = &lsopt->u.sockaddr;

    switch (sa->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        off = offsetof(struct sockaddr_in6, sin6_addr);
        len = 16;
        break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:
        off = offsetof(struct sockaddr_un, sun_path);
        len = sizeof(saun->sun_path);
        break;
#endif

    default: /* AF_INET */
        off = offsetof(struct sockaddr_in, sin_addr);
        len = 4;
        break;
    }

    p = lsopt->u.sockaddr_data + off;

    addr = port->addrs.elts;

    for (i = 0; i < port->addrs.nelts; i++) {

        if (ngx_memcmp(p, addr[i].opt.u.sockaddr_data + off, len) != 0) {
            continue;
        }

        /* the address is already in the address list */

        if (ngx_http_add_server(cf, cscf, &addr[i]) != NGX_OK) {
            return NGX_ERROR;
        }

        /* preserve default_server bit during listen options overwriting */
        default_server = addr[i].opt.default_server;

        proxy_protocol = lsopt->proxy_protocol || addr[i].opt.proxy_protocol;

#if (NGX_HTTP_SSL)
        ssl = lsopt->ssl || addr[i].opt.ssl;
#endif
#if (NGX_HTTP_SPDY)
        spdy = lsopt->spdy || addr[i].opt.spdy;
#endif

        if (lsopt->set) {

            if (addr[i].opt.set) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "duplicate listen options for %s", addr[i].opt.addr);
                return NGX_ERROR;
            }

            addr[i].opt = *lsopt;
        }

        /* check the duplicate "default" server for this address:port */

        if (lsopt->default_server) {

            if (default_server) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "a duplicate default server for %s", addr[i].opt.addr);
                return NGX_ERROR;
            }

            default_server = 1;
            addr[i].default_server = cscf;
        }

        addr[i].opt.default_server = default_server;
        addr[i].opt.proxy_protocol = proxy_protocol;
#if (NGX_HTTP_SSL)
        addr[i].opt.ssl = ssl;
#endif
#if (NGX_HTTP_SPDY)
        addr[i].opt.spdy = spdy;
#endif

        return NGX_OK;
    }

    /* add the address to the addresses list that bound to this port */

    return ngx_http_add_address(cf, cscf, port, lsopt);
}


/*
 * add the server address, the server names and the server core module
 * configurations to the port list
 */

static ngx_int_t
ngx_http_add_address(ngx_conf_t *cf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_conf_port_t *port, ngx_http_listen_opt_t *lsopt)
{
    ngx_http_conf_addr_t  *addr;

    if (port->addrs.elts == NULL) {
        if (ngx_array_init(&port->addrs, cf->temp_pool, 4,
                           sizeof(ngx_http_conf_addr_t))
            != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

#if (NGX_HTTP_SPDY && NGX_HTTP_SSL                                            \
     && !defined TLSEXT_TYPE_application_layer_protocol_negotiation           \
     && !defined TLSEXT_TYPE_next_proto_neg)
    if (lsopt->spdy && lsopt->ssl) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "nginx was built without OpenSSL ALPN or NPN "
                           "support, SPDY is not enabled for %s", lsopt->addr);
    }
#endif

    addr = ngx_array_push(&port->addrs);
    if (addr == NULL) {
        return NGX_ERROR;
    }

    addr->opt = *lsopt;
    addr->hash.buckets = NULL;
    addr->hash.size = 0;
    addr->wc_head = NULL;
    addr->wc_tail = NULL;
#if (NGX_PCRE)
    addr->nregex = 0;
    addr->regex = NULL;
#endif
    addr->default_server = cscf;
    addr->servers.elts = NULL;

    return ngx_http_add_server(cf, cscf, addr);
}


/* add the server core module configuration to the address:port */

static ngx_int_t
ngx_http_add_server(ngx_conf_t *cf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_conf_addr_t *addr)
{
    ngx_uint_t                  i;
    ngx_http_core_srv_conf_t  **server;

    if (addr->servers.elts == NULL) {
        if (ngx_array_init(&addr->servers, cf->temp_pool, 4,
                           sizeof(ngx_http_core_srv_conf_t *))
            != NGX_OK)
        {
            return NGX_ERROR;
        }

    } else {
        server = addr->servers.elts;
        for (i = 0; i < addr->servers.nelts; i++) {
            if (server[i] == cscf) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "a duplicate listen %s", addr->opt.addr);
                return NGX_ERROR;
            }
        }
    }

    server = ngx_array_push(&addr->servers);
    if (server == NULL) {
        return NGX_ERROR;
    }

    *server = cscf;

    return NGX_OK;
}


/*
 * 1.通过ports构造域名的hash结构
 * 2.将posts中所有的监听地址,放入cf->cycle->listening数组中
 *
 * ports:以端口维度保存的整个nginx的监听地址
 * 		8080:{ // 端口
 * 			192.168.146.80:{ // 地址
 * 				www.jd.com // 域名
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
static ngx_int_t
ngx_http_optimize_servers(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf,
    ngx_array_t *ports)
{
    ngx_uint_t             p, a;
    ngx_http_conf_port_t  *port;
    ngx_http_conf_addr_t  *addr;

    if (ports == NULL) {
        return NGX_OK;
    }

    port = ports->elts;
    for (p = 0; p < ports->nelts; p++) {

        ngx_sort(port[p].addrs.elts, (size_t) port[p].addrs.nelts,
                 sizeof(ngx_http_conf_addr_t), ngx_http_cmp_conf_addrs);

        /*
         * check whether all name-based servers have the same
         * configuration as a default server for given address:port
         */

        addr = port[p].addrs.elts;
        for (a = 0; a < port[p].addrs.nelts; a++) {

            if (addr[a].servers.nelts > 1
#if (NGX_PCRE)
            		/* TODO 这个值在什么情况下是1 */
                || addr[a].default_server->captures
#endif
               )
            {
            	/*
            	 * 只有同一个地址对应对个server{}的时候才会走到这里,比如:
            	 * 		server {
            	 *			listen 127.0.0.1:80;
            	 *			server_name xxx;
            	 * 		}
            	 *
            	 *		server {
            	 *			listen 127.0.0.1:80;
            	 *			server_name yyy;
            	 *		}
            	 * 这个时候相当于同一个地址(127.0.0.1:80)包含了两个server{}块,也就是addr[a].servers.nelts等于2,这个时候
            	 * 就需要用域名来区分是哪一个server{}块,所以就需要构造用于匹配域名的hash结构体
            	 *
            	 * 但是当同一个地址只对应一个server{}块的时候,就不需要构造hash结构体了,因为就一个还匹配个屁呀,比如:
            	 * 		server {
            	 * 			listen 127.0.0.1:80;
            	 * 			server_name xxx;
            	 * 		}
            	 *
            	 * 		server {
            	 * 			listen 127.0.0.1:8080;
            	 * 			server_name yyy;
            	 * 		}
            	 * 这里虽然有两个server{}块,但是每个server{}块对应的地址是不相同的,所以就不需要为这两个server{}块中的域名构造hash结构
            	 *
            	 */
                if (ngx_http_server_names(cf, cmcf, &addr[a]) != NGX_OK) {
                    return NGX_ERROR;
                }
            }
        }

        /*
         * 将port[p]中代表的监听地址,放入到cf->cycle->listening数组中
         * 在ngx_http_add_listening()-->ngx_create_listening()方法中操作cf->cycle->listening数组
         */
        if (ngx_http_init_listening(cf, &port[p]) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


/*
 * 将某个端口对应的所有域名放入到ngx_http_conf_addr_t结构体的不同字段中
 *
 * addr->hash: 存放不带通配符的域名(如: www.jd.com)
 * addr->wc_head: 存放带前置通配符的域名(如: *.jd.com)
 * addr->wc_tail: 存放带后置通配符的域名(如: www.jd.*)
 * addr->regex: 存放正则匹配的域名(如: www.\d.com)
 *
 *
 * 只有同一个地址对应对个server{}的时候才会用到这个方法,比如:
 * 		server {
 *			listen 127.0.0.1:80;
 *			server_name xxx;
 * 		}
 *
 *		server {
 *			listen 127.0.0.1:80;
 *			server_name yyy;
 *		}
 * 这个时候相当于同一个地址(127.0.0.1:80)包含了两个server{}块,也就是addr[a].servers.nelts等于2,这个时候
 * 就需要用域名来区分是哪一个server{}块,所以就需要构造用于匹配域名的hash结构体
 *
 * 但是当同一个地址只对应一个server{}块的时候,就不需要构造hash结构体了,因为就一个还匹配个屁呀,比如:
 * 		server {
 * 			listen 127.0.0.1:80;
 * 			server_name xxx;
 * 		}
 *
 * 		server {
 * 			listen 127.0.0.1:8080;
 * 			server_name yyy;
 * 		}
 * 这里虽然有两个server{}块,但是每个server{}块对应的地址是不相同的,所以就不需要为这两个server{}块中的域名构造hash结构
 *
 */
static ngx_int_t
ngx_http_server_names(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf,
    ngx_http_conf_addr_t *addr)
{
    ngx_int_t                   rc;
    ngx_uint_t                  n, s;
    ngx_hash_init_t             hash;
    ngx_hash_keys_arrays_t      ha;
    ngx_http_server_name_t     *name;
    ngx_http_core_srv_conf_t  **cscfp;
#if (NGX_PCRE)
    ngx_uint_t                  regex, i;

    regex = 0;
#endif

    ngx_memzero(&ha, sizeof(ngx_hash_keys_arrays_t));

    ha.temp_pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, cf->log);
    if (ha.temp_pool == NULL) {
        return NGX_ERROR;
    }

    ha.pool = cf->pool;

    // 初始化ngx_hash_keys_arrays_t对象,为下面创建各种hash结构做准备
    if (ngx_hash_keys_array_init(&ha, NGX_HASH_LARGE) != NGX_OK) {
        goto failed;
    }

    /*
     * 假设配置文件如下:
     * 	 server { // server1
     * 	 	listen		8080;
     * 	 	server_name host1 host2;
     * 	 }
     *
     * 	 server { // server2
     * 	 	listen		8080;
     * 	 	server_name host3 host4;
     * 	 }
     *
     *
     * 	 则addr->servers.elts放的数据如下:
     * 	   cscfp[0]	 server1
     * 	   cscfp[1]	 server2
     */
    cscfp = addr->servers.elts;

    /*
     * 下面这个循环主要用来为后续创建域名匹配hash结构准备数据
     */
    for (s = 0; s < addr->servers.nelts; s++) {

    	/*
    	 * 存放cscfp[s](ngx_http_core_srv_conf_t)下的所有域名的数组
    	 */
        name = cscfp[s]->server_names.elts;

        /*
         * 构造cscfp[s](ngx_http_core_srv_conf_t)下的所有域名的hash结构
         */
        for (n = 0; n < cscfp[s]->server_names.nelts; n++) {

#if (NGX_PCRE)
            if (name[n].regex) {
                regex++;
                continue;
            }
#endif

            /*
             * 该方法会根据域名的设置情况将其放入到ngx_hash_keys_arrays_t的不同字段上
             * 	keys:一个数组,存放不带统配符的域名(如 www.jd.com)
             * 	dns_wc_head:一个数组,存放前置通配符的域名(如 *.jd.com)
             * 	dns_wc_tail:一个数组,存放后置通配符的域名(如 www.jd.*)
             */
            rc = ngx_hash_add_key(&ha, &name[n].name, name[n].server,
                                  NGX_HASH_WILDCARD_KEY);

            if (rc == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (rc == NGX_DECLINED) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                              "invalid server name or wildcard \"%V\" on %s",
                              &name[n].name, addr->opt.addr);
                return NGX_ERROR;
            }

            if (rc == NGX_BUSY) {
                ngx_log_error(NGX_LOG_WARN, cf->log, 0,
                              "conflicting server name \"%V\" on %s, ignored",
                              &name[n].name, addr->opt.addr);
            }
        }
    }

    hash.key = ngx_hash_key_lc;
    hash.max_size = cmcf->server_names_hash_max_size;
    hash.bucket_size = cmcf->server_names_hash_bucket_size;
    hash.name = "server_names_hash";
    hash.pool = cf->pool;

    if (ha.keys.nelts) {
        hash.hash = &addr->hash;
        hash.temp_pool = NULL;

        /*
         * 将不带统配符的域名,放入到&addr->hash结构中
         */
        if (ngx_hash_init(&hash, ha.keys.elts, ha.keys.nelts) != NGX_OK) {
            goto failed;
        }
    }

    if (ha.dns_wc_head.nelts) {

    	/*
    	 * 将前置通配符域名,放入到addr->wc_head结构中
    	 */

        ngx_qsort(ha.dns_wc_head.elts, (size_t) ha.dns_wc_head.nelts,
                  sizeof(ngx_hash_key_t), ngx_http_cmp_dns_wildcards);

        /*
         * 将hash.hash置成空,确保ngx_hash_wildcard_init()方法在间接调用ngx_hash_init()方法时
         *    hinit->hash = ngx_pcalloc(hinit->pool, sizeof(ngx_hash_wildcard_t) + size * sizeof(ngx_hash_elt_t *));
         * 最前面是一个ngx_hash_wildcard_t结构体大小的空间
         * 注意上面的hinit就是下面的hash地址,也就是传入ngx_hash_wildcard_init()方法的&hash
         */
        hash.hash = NULL;
        hash.temp_pool = ha.temp_pool;

        if (ngx_hash_wildcard_init(&hash, ha.dns_wc_head.elts,
                                   ha.dns_wc_head.nelts)
            != NGX_OK)
        {
            goto failed;
        }

        addr->wc_head = (ngx_hash_wildcard_t *) hash.hash;
    }

    if (ha.dns_wc_tail.nelts) {
    	/*
		 * 将后置通配符域名,放入到addr->wc_tail结构中
		 */

        ngx_qsort(ha.dns_wc_tail.elts, (size_t) ha.dns_wc_tail.nelts,
                  sizeof(ngx_hash_key_t), ngx_http_cmp_dns_wildcards);

        hash.hash = NULL;
        hash.temp_pool = ha.temp_pool;

        if (ngx_hash_wildcard_init(&hash, ha.dns_wc_tail.elts,
                                   ha.dns_wc_tail.nelts)
            != NGX_OK)
        {
            goto failed;
        }

        addr->wc_tail = (ngx_hash_wildcard_t *) hash.hash;
    }

    ngx_destroy_pool(ha.temp_pool);

#if (NGX_PCRE)

    if (regex == 0) {
        return NGX_OK;
    }

    /*
     * 处理当前端口中,带正则表达式的域名
     * addr->nregex: 带正则域名的个数
     */
    addr->nregex = regex;
    /*
     * 为addr->regex分配regex个存放ngx_http_server_name_t结构的内存,分配后结构如下:
     * 	 addr->regex
     * 	   -----
     * 	   | * |
     * 	   -----
     *		 \					regex个
     *		 -----------------------------------------------------
     *		 |ngx_http_server_name_t| ... |ngx_http_server_name_t|
     *		 -----------------------------------------------------
     *
     */
    addr->regex = ngx_palloc(cf->pool, regex * sizeof(ngx_http_server_name_t));
    if (addr->regex == NULL) {
        return NGX_ERROR;
    }

    i = 0;

    for (s = 0; s < addr->servers.nelts; s++) {

        name = cscfp[s]->server_names.elts;

        for (n = 0; n < cscfp[s]->server_names.nelts; n++) {
            if (name[n].regex) {
                addr->regex[i++] = name[n];
            }
        }
    }

#endif

    return NGX_OK;

failed:

    ngx_destroy_pool(ha.temp_pool);

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_cmp_conf_addrs(const void *one, const void *two)
{
    ngx_http_conf_addr_t  *first, *second;

    first = (ngx_http_conf_addr_t *) one;
    second = (ngx_http_conf_addr_t *) two;

    if (first->opt.wildcard) {
        /* a wildcard address must be the last resort, shift it to the end */
        return 1;
    }

    if (second->opt.wildcard) {
        /* a wildcard address must be the last resort, shift it to the end */
        return -1;
    }

    if (first->opt.bind && !second->opt.bind) {
        /* shift explicit bind()ed addresses to the start */
        return -1;
    }

    if (!first->opt.bind && second->opt.bind) {
        /* shift explicit bind()ed addresses to the start */
        return 1;
    }

    /* do not sort by default */

    return 0;
}


static int ngx_libc_cdecl
ngx_http_cmp_dns_wildcards(const void *one, const void *two)
{
    ngx_hash_key_t  *first, *second;

    first = (ngx_hash_key_t *) one;
    second = (ngx_hash_key_t *) two;

    return ngx_dns_strcmp(first->key.data, second->key.data);
}


/*
 * 将port下代表的所有监听地址,加入到cf->cycle->listeing数组中
 * 每一个listeing都会引用一个代表当前端口下的所有地址的集合
 *
 * 首先每个ngx_listening_t都会有自身的ip和端口(sockaddr),他的servers字段又以端口
 * 维度包含了该端口下的所有地址(ip)
 */
static ngx_int_t
ngx_http_init_listening(ngx_conf_t *cf, ngx_http_conf_port_t *port)
{
    ngx_uint_t                 i, last, bind_wildcard;
    ngx_listening_t           *ls;
    ngx_http_port_t           *hport;
    ngx_http_conf_addr_t      *addr;

    addr = port->addrs.elts;
    last = port->addrs.nelts;

    /*
     * If there is a binding to an "*:port" then we need to bind() to
     * the "*:port" only and ignore other implicit bindings.  The bindings
     * have been already sorted: explicit bindings are on the start, then
     * implicit bindings go, and wildcard binding is in the end.
     */

    if (addr[last - 1].opt.wildcard) {
    	// 因为已经排过序了,带通配符的地址排在最后,所以如果最后一个地址带通配符,那么就需要处理通配符的情况

        addr[last - 1].opt.bind = 1;
        bind_wildcard = 1;

    } else {
        bind_wildcard = 0;
    }

    i = 0;

    while (i < last) {

        if (bind_wildcard && !addr[i].opt.bind) {
            i++;
            continue;
        }

        // addr中包含了sockaddr(ip+port)
        ls = ngx_http_add_listening(cf, &addr[i]);
        if (ls == NULL) {
            return NGX_ERROR;
        }

        hport = ngx_pcalloc(cf->pool, sizeof(ngx_http_port_t));
        if (hport == NULL) {
            return NGX_ERROR;
        }

        ls->servers = hport;

        // 端口下地址的个数
        hport->naddrs = i + 1;

        switch (ls->sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            if (ngx_http_add_addrs6(cf, hport, addr) != NGX_OK) {
                return NGX_ERROR;
            }
            break;
#endif
        default: /* AF_INET */
        	/*        	 *
        	 * 这个方法会把port端口下的所有地址都放入到hport中,而每一个servers都会包含一个hport,
        	 * 所以每一个ls都会包该ls对应的所有地址
        	 */
            if (ngx_http_add_addrs(cf, hport, addr) != NGX_OK) {
                return NGX_ERROR;
            }
            break;
        }

        // 对reuserport的支持,暂时不考虑看
        if (ngx_clone_listening(cf, ls) != NGX_OK) {
            return NGX_ERROR;
        }

        addr++;
        last--;
    }

    return NGX_OK;
}


static ngx_listening_t *
ngx_http_add_listening(ngx_conf_t *cf, ngx_http_conf_addr_t *addr)
{
    ngx_listening_t           *ls;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_http_core_srv_conf_t  *cscf;

    ls = ngx_create_listening(cf, &addr->opt.u.sockaddr, addr->opt.socklen);
    if (ls == NULL) {
        return NULL;
    }

    ls->addr_ntop = 1;

    ls->handler = ngx_http_init_connection;

    cscf = addr->default_server;
    ls->pool_size = cscf->connection_pool_size;
    ls->post_accept_timeout = cscf->client_header_timeout;

    clcf = cscf->ctx->loc_conf[ngx_http_core_module.ctx_index];

    ls->logp = clcf->error_log;
    ls->log.data = &ls->addr_text;
    ls->log.handler = ngx_accept_log_error;

#if (NGX_WIN32)
    {
    ngx_iocp_conf_t  *iocpcf = NULL;

    if (ngx_get_conf(cf->cycle->conf_ctx, ngx_events_module)) {
        iocpcf = ngx_event_get_conf(cf->cycle->conf_ctx, ngx_iocp_module);
    }
    if (iocpcf && iocpcf->acceptex_read) {
        ls->post_accept_buffer_size = cscf->client_header_buffer_size;
    }
    }
#endif

    ls->backlog = addr->opt.backlog;
    ls->rcvbuf = addr->opt.rcvbuf;
    ls->sndbuf = addr->opt.sndbuf;

    ls->keepalive = addr->opt.so_keepalive;
#if (NGX_HAVE_KEEPALIVE_TUNABLE)
    ls->keepidle = addr->opt.tcp_keepidle;
    ls->keepintvl = addr->opt.tcp_keepintvl;
    ls->keepcnt = addr->opt.tcp_keepcnt;
#endif

#if (NGX_HAVE_DEFERRED_ACCEPT && defined SO_ACCEPTFILTER)
    ls->accept_filter = addr->opt.accept_filter;
#endif

#if (NGX_HAVE_DEFERRED_ACCEPT && defined TCP_DEFER_ACCEPT)
    ls->deferred_accept = addr->opt.deferred_accept;
#endif

#if (NGX_HAVE_INET6 && defined IPV6_V6ONLY)
    ls->ipv6only = addr->opt.ipv6only;
#endif

#if (NGX_HAVE_SETFIB)
    ls->setfib = addr->opt.setfib;
#endif

#if (NGX_HAVE_TCP_FASTOPEN)
    ls->fastopen = addr->opt.fastopen;
#endif

#if (NGX_HAVE_REUSEPORT)
    ls->reuseport = addr->opt.reuseport;
#endif

    return ls;
}


/*
 * 将每个端口下的所有地址,存放到hport中
 */
static ngx_int_t
ngx_http_add_addrs(ngx_conf_t *cf, ngx_http_port_t *hport,
    ngx_http_conf_addr_t *addr)
{
    ngx_uint_t                 i;
    ngx_http_in_addr_t        *addrs;
    struct sockaddr_in        *sin;
    ngx_http_virtual_names_t  *vn;

    // 为这个端口创建一个存放所有地址的内存空间
    hport->addrs = ngx_pcalloc(cf->pool,
                               hport->naddrs * sizeof(ngx_http_in_addr_t));
    if (hport->addrs == NULL) {
        return NGX_ERROR;
    }

    addrs = hport->addrs;

    // 把addr数组中所有的地址(只代表某一个端口的)放入到addrs中
    for (i = 0; i < hport->naddrs; i++) {

        sin = &addr[i].opt.u.sockaddr_in;
        addrs[i].addr = sin->sin_addr.s_addr;
        addrs[i].conf.default_server = addr[i].default_server;
#if (NGX_HTTP_SSL)
        addrs[i].conf.ssl = addr[i].opt.ssl;
#endif
#if (NGX_HTTP_SPDY)
        addrs[i].conf.spdy = addr[i].opt.spdy;
#endif
        addrs[i].conf.proxy_protocol = addr[i].opt.proxy_protocol;

        if (addr[i].hash.buckets == NULL
            && (addr[i].wc_head == NULL
                || addr[i].wc_head->hash.buckets == NULL)
            && (addr[i].wc_tail == NULL
                || addr[i].wc_tail->hash.buckets == NULL)
#if (NGX_PCRE)
            && addr[i].nregex == 0
#endif
            )
        {
            continue;
        }

        vn = ngx_palloc(cf->pool, sizeof(ngx_http_virtual_names_t));
        if (vn == NULL) {
            return NGX_ERROR;
        }

        addrs[i].conf.virtual_names = vn;

        vn->names.hash = addr[i].hash;
        vn->names.wc_head = addr[i].wc_head;
        vn->names.wc_tail = addr[i].wc_tail;
#if (NGX_PCRE)
        vn->nregex = addr[i].nregex;
        vn->regex = addr[i].regex;
#endif
    }

    return NGX_OK;
}


#if (NGX_HAVE_INET6)

static ngx_int_t
ngx_http_add_addrs6(ngx_conf_t *cf, ngx_http_port_t *hport,
    ngx_http_conf_addr_t *addr)
{
    ngx_uint_t                 i;
    ngx_http_in6_addr_t       *addrs6;
    struct sockaddr_in6       *sin6;
    ngx_http_virtual_names_t  *vn;

    hport->addrs = ngx_pcalloc(cf->pool,
                               hport->naddrs * sizeof(ngx_http_in6_addr_t));
    if (hport->addrs == NULL) {
        return NGX_ERROR;
    }

    addrs6 = hport->addrs;

    for (i = 0; i < hport->naddrs; i++) {

        sin6 = &addr[i].opt.u.sockaddr_in6;
        addrs6[i].addr6 = sin6->sin6_addr;
        addrs6[i].conf.default_server = addr[i].default_server;
#if (NGX_HTTP_SSL)
        addrs6[i].conf.ssl = addr[i].opt.ssl;
#endif
#if (NGX_HTTP_SPDY)
        addrs6[i].conf.spdy = addr[i].opt.spdy;
#endif

        if (addr[i].hash.buckets == NULL
            && (addr[i].wc_head == NULL
                || addr[i].wc_head->hash.buckets == NULL)
            && (addr[i].wc_tail == NULL
                || addr[i].wc_tail->hash.buckets == NULL)
#if (NGX_PCRE)
            && addr[i].nregex == 0
#endif
            )
        {
            continue;
        }

        vn = ngx_palloc(cf->pool, sizeof(ngx_http_virtual_names_t));
        if (vn == NULL) {
            return NGX_ERROR;
        }

        addrs6[i].conf.virtual_names = vn;

        vn->names.hash = addr[i].hash;
        vn->names.wc_head = addr[i].wc_head;
        vn->names.wc_tail = addr[i].wc_tail;
#if (NGX_PCRE)
        vn->nregex = addr[i].nregex;
        vn->regex = addr[i].regex;
#endif
    }

    return NGX_OK;
}

#endif


char *
ngx_http_types_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_array_t     **types;
    ngx_str_t        *value, *default_type;
    ngx_uint_t        i, n, hash;
    ngx_hash_key_t   *type;

    types = (ngx_array_t **) (p + cmd->offset);

    if (*types == (void *) -1) {
        return NGX_CONF_OK;
    }

    default_type = cmd->post;

    if (*types == NULL) {
        *types = ngx_array_create(cf->temp_pool, 1, sizeof(ngx_hash_key_t));
        if (*types == NULL) {
            return NGX_CONF_ERROR;
        }

        if (default_type) {
            type = ngx_array_push(*types);
            if (type == NULL) {
                return NGX_CONF_ERROR;
            }

            type->key = *default_type;
            type->key_hash = ngx_hash_key(default_type->data,
                                          default_type->len);
            type->value = (void *) 4;
        }
    }

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (value[i].len == 1 && value[i].data[0] == '*') {
            *types = (void *) -1;
            return NGX_CONF_OK;
        }

        hash = ngx_hash_strlow(value[i].data, value[i].data, value[i].len);
        value[i].data[value[i].len] = '\0';

        type = (*types)->elts;
        for (n = 0; n < (*types)->nelts; n++) {

            if (ngx_strcmp(value[i].data, type[n].key.data) == 0) {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                                   "duplicate MIME type \"%V\"", &value[i]);
                goto next;
            }
        }

        type = ngx_array_push(*types);
        if (type == NULL) {
            return NGX_CONF_ERROR;
        }

        type->key = value[i];
        type->key_hash = hash;
        type->value = (void *) 4;

    next:

        continue;
    }

    return NGX_CONF_OK;
}


char *
ngx_http_merge_types(ngx_conf_t *cf, ngx_array_t **keys, ngx_hash_t *types_hash,
    ngx_array_t **prev_keys, ngx_hash_t *prev_types_hash,
    ngx_str_t *default_types)
{
    ngx_hash_init_t  hash;

    if (*keys) {

        if (*keys == (void *) -1) {
            return NGX_CONF_OK;
        }

        hash.hash = types_hash;
        hash.key = NULL;
        hash.max_size = 2048;
        hash.bucket_size = 64;
        hash.name = "test_types_hash";
        hash.pool = cf->pool;
        hash.temp_pool = NULL;

        if (ngx_hash_init(&hash, (*keys)->elts, (*keys)->nelts) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    if (prev_types_hash->buckets == NULL) {

        if (*prev_keys == NULL) {

            if (ngx_http_set_default_types(cf, prev_keys, default_types)
                != NGX_OK)
            {
                return NGX_CONF_ERROR;
            }

        } else if (*prev_keys == (void *) -1) {
            *keys = *prev_keys;
            return NGX_CONF_OK;
        }

        hash.hash = prev_types_hash;
        hash.key = NULL;
        hash.max_size = 2048;
        hash.bucket_size = 64;
        hash.name = "test_types_hash";
        hash.pool = cf->pool;
        hash.temp_pool = NULL;

        if (ngx_hash_init(&hash, (*prev_keys)->elts, (*prev_keys)->nelts)
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    *types_hash = *prev_types_hash;

    return NGX_CONF_OK;

}


ngx_int_t
ngx_http_set_default_types(ngx_conf_t *cf, ngx_array_t **types,
    ngx_str_t *default_type)
{
    ngx_hash_key_t  *type;

    *types = ngx_array_create(cf->temp_pool, 1, sizeof(ngx_hash_key_t));
    if (*types == NULL) {
        return NGX_ERROR;
    }

    while (default_type->len) {

        type = ngx_array_push(*types);
        if (type == NULL) {
            return NGX_ERROR;
        }

        type->key = *default_type;
        type->key_hash = ngx_hash_key(default_type->data,
                                      default_type->len);
        type->value = (void *) 4;

        default_type++;
    }

    return NGX_OK;
}
