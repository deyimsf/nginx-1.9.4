
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * gzip打开,关闭chunked编码(chunked_transfer_encoding=off),则不会输出Content-Length响应头,
 * 那客户端是怎么知道服务端内容输出完毕的呢?
 * 好像就不知道了,ngx会自己控制,输出完毕后不关是长连接还是短连接,都会直接关闭连接,依次告知响应完毕.
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_chain_t         *free;
    ngx_chain_t         *busy;
} ngx_http_chunked_filter_ctx_t;


static ngx_int_t ngx_http_chunked_filter_init(ngx_conf_t *cf);


static ngx_http_module_t  ngx_http_chunked_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_chunked_filter_init,          /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_chunked_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_chunked_filter_module_ctx,   /* module context */
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


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_int_t
ngx_http_chunked_header_filter(ngx_http_request_t *r)
{
    ngx_http_core_loc_conf_t       *clcf;
    ngx_http_chunked_filter_ctx_t  *ctx;

    if (r->headers_out.status == NGX_HTTP_NOT_MODIFIED
        || r->headers_out.status == NGX_HTTP_NO_CONTENT
        || r->headers_out.status < NGX_HTTP_OK
        || r != r->main  // 子请求不走chunk过滤器
        || (r->method & NGX_HTTP_HEAD))
    {
        return ngx_http_next_header_filter(r);
    }

    if (r->headers_out.content_length_n == -1) {
    	/*
    	 * 在该过滤器前面的逻辑把headers_out.content_length_n被设置成-1才有可能走chunked编码
    	 *
    	 * 比如gzip模块会调用ngx_http_clear_content_length(r)方法把content_length_n设置为-1
    	 *
    	 * 对于range方式的请求是不会走chunked编码的，因为range对应的头过滤器把headers_out.content_length_n设置了请求头中的长度(Range:bytes=start-end)
    	 */

        if (r->http_version < NGX_HTTP_VERSION_11) {
            r->keepalive = 0;

        } else {
            clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

            if (clcf->chunked_transfer_encoding) {
                r->chunked = 1;

                ctx = ngx_pcalloc(r->pool,
                                  sizeof(ngx_http_chunked_filter_ctx_t));
                if (ctx == NULL) {
                    return NGX_ERROR;
                }

                ngx_http_set_ctx(r, ctx, ngx_http_chunked_filter_module);

            } else {
                r->keepalive = 0;
            }
        }
    }

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_chunked_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    u_char                         *chunk;
    off_t                           size;
    ngx_int_t                       rc;
    ngx_buf_t                      *b;
    ngx_chain_t                    *out, *cl, *tl, **ll;
    ngx_http_chunked_filter_ctx_t  *ctx;

    if (in == NULL || !r->chunked || r->header_only) {
        return ngx_http_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_chunked_filter_module);

    out = NULL;
    ll = &out;

    size = 0;
    cl = in;

    for ( ;; ) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http chunk: %d", ngx_buf_size(cl->buf));

        size += ngx_buf_size(cl->buf);

        if (cl->buf->flush
            || cl->buf->sync
            || ngx_buf_in_memory(cl->buf)
            || cl->buf->in_file)
        {
            tl = ngx_alloc_chain_link(r->pool);
            if (tl == NULL) {
                return NGX_ERROR;
            }

            tl->buf = cl->buf;
            *ll = tl;
            ll = &tl->next;
        }

        if (cl->next == NULL) {
            break;
        }

        cl = cl->next;
    }

    if (size) {
        tl = ngx_chain_get_free_buf(r->pool, &ctx->free);
        if (tl == NULL) {
            return NGX_ERROR;
        }

        b = tl->buf;
        chunk = b->start;

        if (chunk == NULL) {
            /* the "0000000000000000" is 64-bit hexadecimal string */

            chunk = ngx_palloc(r->pool, sizeof("0000000000000000" CRLF) - 1);
            if (chunk == NULL) {
                return NGX_ERROR;
            }

            b->start = chunk;
            b->end = chunk + sizeof("0000000000000000" CRLF) - 1;
        }

        b->tag = (ngx_buf_tag_t) &ngx_http_chunked_filter_module;
        b->memory = 0;
        b->temporary = 1;
        b->pos = chunk;
        b->last = ngx_sprintf(chunk, "%xO" CRLF, size);

        tl->next = out;
        out = tl;
    }

    if (cl->buf->last_buf) {
        tl = ngx_chain_get_free_buf(r->pool, &ctx->free);
        if (tl == NULL) {
            return NGX_ERROR;
        }

        b = tl->buf;

        b->tag = (ngx_buf_tag_t) &ngx_http_chunked_filter_module;
        b->temporary = 0; // 不能修改这块缓存，因为长度已经确定了(看上面代码：ngx_sprintf(chunk, "%xO" CRLF, size);)
        b->memory = 1;
        b->last_buf = 1;
        b->pos = (u_char *) CRLF "0" CRLF CRLF;
        b->last = b->pos + 7;

        cl->buf->last_buf = 0;

        *ll = tl;

        if (size == 0) {
            b->pos += 2;
        }

    } else if (size > 0) {
        tl = ngx_chain_get_free_buf(r->pool, &ctx->free);
        if (tl == NULL) {
            return NGX_ERROR;
        }

        b = tl->buf;

        b->tag = (ngx_buf_tag_t) &ngx_http_chunked_filter_module;
        b->temporary = 0; // 不能修改这块缓存，因为长度已经确定了(看上面代码：ngx_sprintf(chunk, "%xO" CRLF, size);)
        b->memory = 1;
        b->pos = (u_char *) CRLF;
        b->last = b->pos + 2;

        *ll = tl;

    } else {
        *ll = NULL;
    }

    rc = ngx_http_next_body_filter(r, out);

    ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &out,
                            (ngx_buf_tag_t) &ngx_http_chunked_filter_module);

    return rc;
}


static ngx_int_t
ngx_http_chunked_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_chunked_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_chunked_body_filter;

    return NGX_OK;
}
