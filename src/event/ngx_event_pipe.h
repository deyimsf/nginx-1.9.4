
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_EVENT_PIPE_H_INCLUDED_
#define _NGX_EVENT_PIPE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


typedef struct ngx_event_pipe_s  ngx_event_pipe_t;

typedef ngx_int_t (*ngx_event_pipe_input_filter_pt)(ngx_event_pipe_t *p,
                                                    ngx_buf_t *buf);
typedef ngx_int_t (*ngx_event_pipe_output_filter_pt)(void *data,
                                                     ngx_chain_t *chain);


struct ngx_event_pipe_s {
	/* 上游链路,比如tomcat服务器 */
    ngx_connection_t  *upstream;
    /* 下游链路,比如客户端的浏览器*/
    ngx_connection_t  *downstream;

    // TODO 干啥的呀
    ngx_chain_t       *free_raw_bufs;

    /*
     * 从链路中读到的数据都会追加到这个链表中
     * 由ngx_event_pipe_read_upstream()方法通过回调p->input_filter()方法来实现
     */
    ngx_chain_t       *in;
    /*
     * 如果cl是链表in的最后一个链表项,那么这个字段指向cl->next这个变量的地址
     *             in	     last_in
     *          ---------     -----
     *          | *next |     | * |
     *			---------     -----
     *				  \cl    /
     *				  ------/--
     *				  | *next |
     *				  ---------
     *				  	 \
     *				  	-------
     *				  	| xxx |
     *				  	-------
     *
     * 此时last_in中的值就是cl->next这个变量的地址
     * cl->next这个变量的地址加*就代表这个变量指向的内容xxx,如果我们要把xxx该为yyy
     * 那么以下两个操作等价:
     * 	 cl->next = yyy
     * 	 *last_in = yyy
     *
     */
    ngx_chain_t      **last_in;

    /*
     * TODO 暂时得到的结论,待验证
     * 这个字段用来存放写到临时文件中的数据,这些数据来自链表in
     * 在向客户端输出的时候要先输出该字段的数据,然后在输出in中的数据
     * 由ngx_event_pipe_write_to_downstream()方法通过回调p->output_filter()方法来实现
     */
    ngx_chain_t       *out;
    ngx_chain_t       *free;
    ngx_chain_t       *busy;

    /*
     * the input filter i.e. that moves HTTP/1.1 chunks
     * from the raw bufs to an incoming chain
     *
     * 这个方法的潜规则是把buf中的数据移动到p->in链中
     * 如果u->buffering=1,则这个方法必须设置
     * 这个方法实现一个数据搬运的过程,类似u->input_filter(),ngx中有很多这个搬运数据的用法
     *
     * /http/modules/ngx_http_fastcgi_module.c:712:    u->pipe->input_filter = ngx_http_fastcgi_input_filter;
     * /http/modules/ngx_http_proxy_module.c:890:    u->pipe->input_filter = ngx_http_proxy_copy_filter;
     * /http/modules/ngx_http_proxy_module.c:1508:    r->upstream->pipe->input_filter = ngx_http_proxy_copy_filter;
     * /http/modules/ngx_http_proxy_module.c:1975:        u->pipe->input_filter = ngx_http_proxy_chunked_filter;
     * /http/modules/ngx_http_scgi_module.c:511:    u->pipe->input_filter = ngx_event_pipe_copy_input_filter;
     * /http/modules/ngx_http_uwsgi_module.c:679:    u->pipe->input_filter = ngx_event_pipe_copy_input_filter;
     */

    ngx_event_pipe_input_filter_pt    input_filter;
    void                             *input_ctx;

    /*
     * 当开启buffering时输出数据使用的方法,目前在ngx_http_upstream_send_response()方法中设置
     * 	 p->output_filter = (ngx_event_pipe_output_filter_pt) ngx_http_output_filter;
     * 通过启动http的过滤器来输出数据
     */
    ngx_event_pipe_output_filter_pt   output_filter;
    /* 存放调用output_filter方法时的入参 */
    void                             *output_ctx;

    unsigned           read:1;
    unsigned           cacheable:1;
    unsigned           single_buf:1;
    unsigned           free_bufs:1;
    unsigned           upstream_done:1;
    unsigned           upstream_error:1;
    unsigned           upstream_eof:1;
    unsigned           upstream_blocked:1;
    unsigned           downstream_done:1;
    unsigned           downstream_error:1;
    unsigned           cyclic_temp_file:1;

    ngx_int_t          allocated;
    ngx_bufs_t         bufs;
    ngx_buf_tag_t      tag;

    ssize_t            busy_size;

    /*
     * 从链路中读到的数据的长度 TODO 一次读到的?
     */
    off_t              read_length;
    off_t              length;

    off_t              max_temp_file_size;
    ssize_t            temp_file_write_size;

    ngx_msec_t         read_timeout;
    ngx_msec_t         send_timeout;
    ssize_t            send_lowat;

    ngx_pool_t        *pool;
    ngx_log_t         *log;


    /*
     * 用来存放先前读到的数据链,在upstream中代表的是upstream中的buffer字段,比如在ngx_http_upstream_send_response()
     * 方法中会有如下设置:
     * 		p->preread_bufs->buf = &u->buffer;
     */
    ngx_chain_t       *preread_bufs;
    /*
     * 数据链preread_bufs的数据大小,比如在ngx_http_upstream_send_response()方法中会有如下设置
     * 		p->preread_size = u->buffer.last - u->buffer.pos;
     */
    size_t             preread_size;
    ngx_buf_t         *buf_to_file;

    size_t             limit_rate;
    time_t             start_sec;

    ngx_temp_file_t   *temp_file;

    /* STUB */ int     num;
};


ngx_int_t ngx_event_pipe(ngx_event_pipe_t *p, ngx_int_t do_write);
ngx_int_t ngx_event_pipe_copy_input_filter(ngx_event_pipe_t *p, ngx_buf_t *buf);
ngx_int_t ngx_event_pipe_add_free_buf(ngx_event_pipe_t *p, ngx_buf_t *b);


#endif /* _NGX_EVENT_PIPE_H_INCLUDED_ */
