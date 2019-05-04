
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_str_t     before_body;
    ngx_str_t     after_body;

    ngx_hash_t    types;
    ngx_array_t  *types_keys;
} ngx_http_addition_conf_t;


typedef struct {
    ngx_uint_t    before_body_sent;
} ngx_http_addition_ctx_t;


static void *ngx_http_addition_create_conf(ngx_conf_t *cf);
static char *ngx_http_addition_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_addition_filter_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_addition_commands[] = {

    { ngx_string("add_before_body"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_addition_conf_t, before_body),
      NULL },

    { ngx_string("add_after_body"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_addition_conf_t, after_body),
      NULL },

    { ngx_string("addition_types"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_types_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_addition_conf_t, types_keys),
      &ngx_http_html_default_types[0] },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_addition_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_addition_filter_init,         /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_addition_create_conf,         /* create location configuration */
    ngx_http_addition_merge_conf           /* merge location configuration */
};


ngx_module_t  ngx_http_addition_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_addition_filter_module_ctx,  /* module context */
    ngx_http_addition_commands,            /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_int_t
ngx_http_addition_header_filter(ngx_http_request_t *r)
{
    ngx_http_addition_ctx_t   *ctx;
    ngx_http_addition_conf_t  *conf;

    if (r->headers_out.status != NGX_HTTP_OK || r != r->main) {
        return ngx_http_next_header_filter(r);
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_addition_filter_module);

    if (conf->before_body.len == 0 && conf->after_body.len == 0) {
        return ngx_http_next_header_filter(r);
    }

    if (ngx_http_test_content_type(r, &conf->types) == NULL) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_addition_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_addition_filter_module);

    /*
     * 因为有子请求所有这里不确定内容长度了,后续就只能使用chunk编码告诉客户端响应体的长度,
     * 如果关闭chunk编码,那ngx就只能用短连接的方式来告知客户端响应体发送完毕
     */
    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_weak_etag(r);

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_addition_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                  rc;
    ngx_uint_t                 last;
    ngx_chain_t               *cl;
    ngx_http_request_t        *sr;
    ngx_http_addition_ctx_t   *ctx;
    ngx_http_addition_conf_t  *conf;

    if (in == NULL || r->header_only) {
        return ngx_http_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_addition_filter_module);

    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_addition_filter_module);

    if (!ctx->before_body_sent) {
        ctx->before_body_sent = 1;

        /*
         * 在向客户端发送主请求的响应之前先发送一个前置子请求,这样客户端会先接收到子请求的数据
         */
        if (conf->before_body.len) {
            if (ngx_http_subrequest(r, &conf->before_body, NULL, &sr, NULL, 0)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            /*
             * 前置子请求发送完毕后r->postponed链表的结构可能是如下状态:
             * 图1:  postponed   r_pp代表当前请求r对应的ngx_http_postponed_request_t对象
             * 		----------
             * 		|  r_pp  |
             *      ----------
             *          \
             *          -----------
             *          | before1 |
             *          -----------
             */

        }
    }

    if (conf->after_body.len == 0) {
        ngx_http_set_ctx(r, NULL, ngx_http_addition_filter_module);

        /*
         * 如果不需要发送后置子请求就直接调用下一个过滤器
         */
        return ngx_http_next_body_filter(r, in);
    }

    last = 0;

    /**
     * 检查是否是请求群中最后一块数据
     */
    for (cl = in; cl; cl = cl->next) {
        if (cl->buf->last_buf) {
            cl->buf->last_buf = 0;
            cl->buf->sync = 1;
            last = 1;
        }
    }

    /*
     * 先调用ngx_http_next_body_filter()方法将主请求的数据发给客户端,然后再发起后置子请求,这样
     * 就可以保证后置子请求的数据发生排在主请求内容之后
     *
     * 这里是使用last这个标志位来保证的,last等于一就表示当前链表in中包含了请求r的所有响应数据,他调用
     * 后续过滤器后就会把in中的数据输出到r->postponed的后面
     *
     */
    rc = ngx_http_next_body_filter(r, in);

    /*
     * 当执行完上面的方法后,r->postponed的状态图可能会像这样:
     * 图2:  postponed   r_pp代表当前请求r对应的ngx_http_postponed_request_t对象
	 *	    ----------
	 * 		|  r_pp  |
	 *      ----------
	 *          \
	 *          -----------       --------
	 *          | before1 | --->  | data |
	 *          -----------       --------
	 * 可以看到图2比图1多了一个data节点,这个节点的数据就是当前请求r输出的数据,因为有子请求before1的存在
	 * 所以他没有把数据直接输出到客户端,而是把数据暂存到了r->postpone链表的最后,只有before1的数据输出
	 * 完毕后才会输出data节点的数据。
	 *
	 * 如果在一开始就没有发起图1中的子请求before1,那么就代表r->postponed链表是NUL,这样也就不会出现一个
	 * data节点来暂存数据,而是和常规请求一样使用r->out来暂存没有发出去的数据。
     */

    if (rc == NGX_ERROR || !last || conf->after_body.len == 0) {

    	/*
    	 * 如果ngx_http_next_body_filter()方法都返回NGX_ERROR了那就不用发送after子请求了
    	 *
    	 * 如果不是后一个buf,那现在还是不发送after子请求,万一中间有一次过滤器返回NGX_ERROR那不就白发了吗
    	 *
    	 * 如果conf->after_body.len == 0也就不用发了,因为根本没有这个after指令.
    	 *
    	 * 另一个需要注意的是因为buf_lasf这个标记代表的是整个请求群的最后一块数据,所以这里也说明一个问题
    	 * add_after_body这个指令不能再嵌套子请求中,比如下面的例子:
    	 * 		location /main {
    	 * 			return 200 "main-->>> ";
    	 * 			add_after_body /sub1;
    	 * 		}
    	 *
    	 * 		location /sub1 {
    	 * 			reutrn 200 "sub1-->>> "
    	 * 			add_after_body /sub2;
    	 * 		}
    	 *
    	 * 		location /sub2 {
    	 * 			reutrn 200 "sub2-->>> "
    	 * 		}
    	 * 当访问/main的时候,/sub1中的add_after_body指令是不起作用的,因为这个指令不能嵌套在子请求中
    	 * 当访问/sub1的时候,/sub1中的add_after_body指令是可以发出去的,因为他没有嵌套在子请求中
    	 *
    	 */
        return rc;
    }

    /*
     * 如果前面存在before1这个子请求,那么当下面的方法执行完毕后,对于postponed可能会是如下的状态:
     *  图3:  postponed   r_pp代表当前请求r对应的ngx_http_postponed_request_t对象
     *		  --------
     *		  | r_pp |
     *		  --------
     *			  \
     *			-----------      --------      ----------
     *			| before1 | ---> | data | ---> | after1 |
     *			-----------      --------      ----------
     * 其中data是一个数据节点,它里面存放了当前请求r中的所有响应数据
     *
     *
     * 如果前面没有发起过前置子请求(before1),那么下面方法执行完毕后,postponed可能会是如下状态:
     * 图4: postponed   r_pp代表当前请求r对应的ngx_http_postponed_request_t对象
     *		  --------
     *		  | r_pp |
     *		  --------
     *			  \
     *		     ----------
     *			 | after1 |
     *			 ----------
     * 可以看到没有数据节点data的存在,因为前请求r的数据都输出到客户端或存放到r->out中了
     */
    if (ngx_http_subrequest(r, &conf->after_body, NULL, &sr, NULL, 0)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    /*
     * 如果当前请求既有前置请求,自己又有数据要输出,又有后置请求,那么在后续的执行过程中可能会有如下状态的postpoed链表:
     * 图5:  postponed   r_pp代表当前请求r对应的ngx_http_postponed_request_t对象
     *  	  --------
     *		  | r_pp |
     *		  --------
     *			  \
     *			+---------+      --------      ----------
     *			| before1 | ---> | data | ---> | after1 |
     *			+---------+      --------      ----------
     *												\
     *											   --------
     *											   | data |
     *											   --------
     * 其中before1是当前可以向客户端输出数据的之请求,他的数据会直接输出到客户端,不会产生一个data节点
     * before1后面的data是请求r_pp要输出的数据
     * after1下面的data是after1要输出的数据
     * 他们的最终输出过程如下:
     * 	before1 --> data(before1后的) --> data(after1下的)
     *
     */

    // 发送完子请求后把上下文置空就行了,这样这个请求后续就不会再走这个过滤器了
    ngx_http_set_ctx(r, NULL, ngx_http_addition_filter_module);

    /*
     * 这里不需要再像其他模块那样调用ngx_http_next_body_filter()方法来结束,因为如果当前是子请求,那么
     * 子请求有自己的一套结束流程,如果当前是父请求,那么在上面已经调用完ngx_http_next_body_filter()方法了
     *
     * 为什么要把父请求的最后一个buf->last_buf设置为0,如果不设置为0这里直接调用ngx_http_output_filter(r, NULL)不行吗?
     * 或者说不设置最后一个buf->last_buf设置为0,而是这里直接使用上面的rc作为返回值?
     *
     * 答案是不行:
     * 因为如果不把buf->last_buf设置为0,那么很有可能当这个父请求执行完毕后就直接关闭了(因为没有其它子请求了),如果父请求
     * 都关闭了也就不可能在产生子请求了,所以才有了下面调用ngx_http_send_special()方法的动作.
     *
     * 前面为了发送after子请求需要先把主请求的数据发送完毕,但是又不能让后续的过滤器看到buf->last_buf标记(看到后请求就可能被关闭),
     * 所以在发送之前把父请求的最后一个cl->buf->last_buf设置成了0,这样父请求即使把数据输出完毕了也不会去结束父请求.
     * 当after子请求发送完毕后,通过调用ngx_http_send_special()方法来解除父请求的调用,ngx_http_send_special()
     * 方法中会创建一个b->last_buf = 1的buf,然后调用ngx_http_output_filter()方法把这个buf传过去,此时父请求在将其所有的子
     * 请求输出完毕后才可以正常结束.
     */
    return ngx_http_send_special(r, NGX_HTTP_LAST);
}


static ngx_int_t
ngx_http_addition_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_addition_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_addition_body_filter;

    return NGX_OK;
}


static void *
ngx_http_addition_create_conf(ngx_conf_t *cf)
{
    ngx_http_addition_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_addition_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->before_body = { 0, NULL };
     *     conf->after_body = { 0, NULL };
     *     conf->types = { NULL };
     *     conf->types_keys = NULL;
     */

    return conf;
}


static char *
ngx_http_addition_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_addition_conf_t *prev = parent;
    ngx_http_addition_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->before_body, prev->before_body, "");
    ngx_conf_merge_str_value(conf->after_body, prev->after_body, "");

    if (ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                             &prev->types_keys, &prev->types,
                             ngx_http_html_default_types)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
