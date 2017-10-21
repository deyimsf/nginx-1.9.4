
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * 两个链表:
 *	posted_requests
 *	postponed
 * 这两个链表存放的都是ngx_http_posted_request_t对象
 *
 * 第一链表posted_requests存放主请求下的所有子请求
 * 第二个链表postponed存放每个请求的直接子请求
 *
 * postponed链表某一时刻的状态图:
 *	    postponed
 *		--------
 *		| main |
 *		--------
 *		   \
 *		   +------+		  --------	    ---------     --------      ---------
 *		   | sub1 | ----> | sub2 | ---> | data1 | --->| sub3 | ---> | data2 |
 *		   +------+		  --------      ---------     --------      ---------
 *							  \							  \
 *							--------    		 		--------      --------
 *							| data | 					| sub4 | ---> | data |
 *							--------    				--------      --------
 *															\
 *															--------
 *															| data |
 *															--------
 * 其中sub1表示该他输出数据了,也就是此时的c->data。
 * 如果在整个请求过程中不会再产生子请求了,那么上图数据的输出顺序如下:
 *  sub1 --> sub2下的data --> sub2后的data1 --> sub4下的data --> sub4后的data --> sub3后的data2
 *
 * ngx_http_finalize_request()方法中的有处理子请求的逻辑,它会向上移动c->data值,比如当sub1数据输出完毕
 * 后把c->data设置为main,后续该过滤器会通过main->postponed的判断来执行并设置c->data为sub2
 *
 * 子请求通过ngx_http_run_posted_requests()方法真正执行,这个方法的作用是遍历r->main->posted_requests
 * 链表中的子请求,然后依次调用对应子请求的写事件方法(write_event_handler)
 *
 * 方法/src/http/ngx_http_request.c/ngx_http_post_request()负责把子请求放入到r->main->posted_requests链表中
 * 用到ngx_http_post_request()方法的地方有:
 * 	  /src/http/ngx_http_core_module.c/ngx_http_subrequest()
 * 	  /src/http/ngx_http_postponed_filter_module.c
 * 等。
 *
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static ngx_int_t ngx_http_postpone_filter_add(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_postpone_filter_init(ngx_conf_t *cf);


static ngx_http_module_t  ngx_http_postpone_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_postpone_filter_init,         /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_postpone_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_postpone_filter_module_ctx,  /* module context */
    NULL,                                  /* module directives */
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


static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


/*
 * 处理子请求的过滤器?
 */
static ngx_int_t
ngx_http_postpone_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_connection_t              *c;
    ngx_http_postponed_request_t  *pr;

    c = r->connection;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http postpone filter \"%V?%V\" %p", &r->uri, &r->args, in);

    // 当前不应该r请求输出数据，只是把获取的数据暂存起来
    /*
     * TODO 关键点
     *
     * c->data是子请求的request对象?
     * 在启动其请求的/src/http/ngx_http_core_module.c/ngx_http_subrequest()方法中会设置
     */
    if (r != c->data) {

        if (in) {
            ngx_http_postpone_filter_add(r, in);
            return NGX_OK;
        }

#if 0
        /* TODO: SSI may pass NULL */
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "http postpone filter NULL inactive request");
#endif

        return NGX_OK;
    }

    /*
     * 如果是主请求,那么刚开始r->postponed一定为空,所以会直接走ngx_http_next_body_filter()方法
     * 走正规流程,最后会走ngx_http_write_filter_module过滤器进行内容输出
     *
     * 能走到这里说明 r == c->data,代表该当前请求向浏览器输出数据了,那么具体该请求能不能向外输出数据
     * 还需要看当前请求中有没有子请求,如果没有就直接输出,有的话就需要先把数据暂存起来
     */

    if (r->postponed == NULL) {
    	/*
    	 * r->postponed为空代表,当前请求中已经不存在子请求了,所以可以把数据直接输出到客户端浏览器了
    	 * 也就是说可以直接向主请求(r->main)输出响应数据了
    	 */
        if (in || c->buffered) {
            return ngx_http_next_body_filter(r->main, in);
        }

        // 没有要输出的数据就直接返回了

        return NGX_OK;
    }

    /*
     * 走到这里说明该当前请求r输出数据了,但是r下面还存在子请求,所以先将该请求的数据暂存起来,放到r->postponed
     * 链表的最后,并把ps->reqeust标志位NULL,代表这是一个数据对象
     */
    if (in) {
        ngx_http_postpone_filter_add(r, in);
    }

    /*
     * 下面这个循环的作用:
     *  1.把当前请求的r->postponed链表中第一个非数据节点,放入到r->main->posted_requests链表中
     *    后续会执行这个非数据节点
     *  2.把当前请求的r->postponed链表中连续的数据节点(遇到非数据节点则执行第一步)输出到客户端
     * 每遍历一个链表项都会更新r->postponed链表,所以该循环也相当于弹出操作
     *
     */
    do {
        pr = r->postponed;

        if (pr->request) {

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter wake \"%V?%V\"",
                           &pr->request->uri, &pr->request->args);

            r->postponed = pr->next;

            /*
             * 当前请求下还存在子请求,应该先输出子请求的数据才能输出当前请求中的数据,所以c->data
             * 设置为该子请求(pr->reqeust)
             */
            c->data = pr->request;

            /*
             * 把子请求(pr->request)放入到r->main->posted_request链表中
             */
            return ngx_http_post_request(pr->request, NULL);
        }

        /*
         * 走到这里说明pr->request == NULL, ngx用pr->request == NULL来代表这是一个数据节点
         * 既然是数据节点那就可以把数据直接输出到客户端了
         */

        if (pr->out == NULL) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "http postpone filter NULL output");

        } else {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter output \"%V?%V\"",
                           &r->uri, &r->args);

            /*
             * 把该数据节点的数据输出到客户端
             */
            if (ngx_http_next_body_filter(r->main, pr->out) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

        /*
         * 跳过这个数据节点,继续向下遍历
         */
        r->postponed = pr->next;

    } while (r->postponed);

    return NGX_OK;
}


static ngx_int_t
ngx_http_postpone_filter_add(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_postponed_request_t  *pr, **ppr;

    if (r->postponed) {
        for (pr = r->postponed; pr->next; pr = pr->next) { /* void */ }

        /*
         *
         */

        if (pr->request == NULL) {
            goto found;
        }

        ppr = &pr->next;

    } else {
        ppr = &r->postponed;
    }

    pr = ngx_palloc(r->pool, sizeof(ngx_http_postponed_request_t));
    if (pr == NULL) {
        return NGX_ERROR;
    }

    *ppr = pr;

    pr->request = NULL;
    pr->out = NULL;
    pr->next = NULL;

found:

    if (ngx_chain_add_copy(r->pool, &pr->out, in) == NGX_OK) {
        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_postpone_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_postpone_filter;

    return NGX_OK;
}
