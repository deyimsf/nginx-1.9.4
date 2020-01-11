
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * 该过滤器只负责处理响应体
 *
 * 这个过滤器的主要作用是处理如何读取文件中的数据,具体逻辑看/src/core/ngx_output_chain.c文件中方法
 *
 * 基本逻辑有以下几种情况:
 *     1.如果数据在文件中,并且不支持sendfile方式,那么就要把文件读取到内存中,然后才能发出去,copy_filter的
 *       大部分逻辑都是在做这个事
 *     2.如果数据在文件中,并且支持sendfile方式,那么就直接把对应的chian和buf传递到下一个过滤器发出去就可以了
 *     3.如果数据在内存中,那就直接把对应的chian和buf传递到下一个过滤器发出去就可以了
 * 这个过滤器依赖于ngx_output_chain_ctx_t结构体,作为当前request的上下文来处理上面的逻辑
 *
 *
 *
 *
 * -------------------限速逻辑在ngx_http_write_filter_module模块中(对应limit_rate指令)---------------------
 * 限速:每次就发limit个字节，发完这limit个字节后，先计算后续应该延迟多长时间delay，让后把对应写事件放入到定期器中，间隔时间就是delay
 *   如果还又数据没有发送完毕，则返回NGX_AGAIN，这样等过去delay时间后，该写事件会再次出发，然后再次动态计算应该发的limit。
 *   (只要有delay，就表示本次一定没有把数据都发送出去？)
 *
 *
 *
 *
 * --------------------------------过滤器的基本执行逻辑，以自带的ngx_http_static_module模块举例---------------------
 *
 * 1.在阶段执行时，当执行到内容handler时，调用如下方法：
 *    ngx_http_core_content_phase();
 *
 * 2.内容handler会调用static模块的如下方法:
 *    ngx_http_static_handler(r)
 *
 * 3.上面的方法又会调用过滤器方法:
 *    ngx_http_output_filter(r, &out)
 *
 * 4.过滤器方法会调用一系列的过滤器:
 *   ngx_http_copy_filter_module.c --> ngx_output_chain(ctx, in) --> ngx_http_write_filter_module.c
 *
 * 5.其中ngx_output_chain(ctx, in)负责把in中要输出的数据追加到ctx->in中，然后该方法又会试着把ctx->in中的数据输出到客户端。
 *
 *   ctx->buf中的buf，会先组成一个有out牵头的链，然后通过调用ctx->output_filter(ctx->filter_ctx, out)方法把数据输出去
 *   (ctx->buf块的个数由output_buffers指令决定)
 *
 * 6.ctx->output_filter(ctx->filter_ctx, out)最终会调用到最后一个过滤器
 *      ngx_http_write_filter_module.c
 *   该过滤器调用的方法是:
 *      ngx_http_write_filter(ngx_http_request_t *r, ngx_chain_t *in)
 *   其中，第一个参数就是ctx->filter_ctx，第二个参数就是out。
 *   该方法会把in中的buf数据追加到r->out链中(这两个链会共用同一个buf，但是不会共用chain，在追加的过程中会生成新的chain放到r->out中)
 *   然后调用如下方法发送数据:
 *      chain = c->send_chain(c, r->out, limit);
 *   如果数据没能一次发送完毕，则调用如下逻辑:
 *      r->out = chain;
 *      if (chain) {
 *         c->buffered |= NGX_HTTP_WRITE_BUFFERED;
 *         return NGX_AGAIN;
 *      }
 *
 * 7.最终上面的返回值会传递到第一步的
 *     ngx_http_core_content_phase()
 *   这个阶段handler会做如下逻辑判断:
 *     if (rc != NGX_DECLINED) {
 *        ngx_http_finalize_request(r, rc);
 *        return NGX_OK;
 *     }
 *   因为此时返回值rc是NGX_AGAIN，所以会走ngx_http_finalize_request()方法
 *
 * 8.ngx_http_finalize_request()方法，该方法根据返回值，最终会执行到如下逻辑:
 *      if (r->buffered || c->buffered || r->postponed || r->blocked) {
 *         if (ngx_http_set_write_handler(r) != NGX_OK) {
 *            ngx_http_terminate_request(r, 0);
 *         }
 *         return;
 *      }
 *    由于在第六步设置了:
 *       c->buffered |= NGX_HTTP_WRITE_BUFFERED;
 *    所以此时会执行
 *       ngx_http_set_write_handler()
 *    该方法的作用是把当前请求的写事件方法设置成如下:
 *       r->write_event_handler = ngx_http_writer;
 *
 * 9.ngx_http_writer()方法每次都会重新启动过滤器
 *      ngx_http_output_filter(r, NULL);
 *   所以它会再次执行第四步的逻辑:
 *      ngx_http_copy_filter_module.c --> ngx_output_chain(ctx, in) --> ngx_http_write_filter_module.c
 *   不过这次第二个参数是空，所以就不存在把in向ctx->in中追加数据的操作了，而是走第五步的其它逻辑。
 *
 * 10.这次等第六步走完后，不管数据是否发送完毕，下次事件到来后继续执行ngx_http_writer()方法。
 *    再次执行ngx_http_writer()方法的时候，如果发现数据已经发送完毕，则直接再次调用结束请求的方法:
 *       ngx_http_finalize_request(r, rc); // 此时rc不等于NGX_AGAIN
 *    此时根据该方法内部逻辑处理该请求
 *
 *    ngx_http_writer()方法判断数据是否发送完毕使用如下逻辑:
 *      if (r->buffered || r->postponed || (r == r->main && c->buffered)) {
 *         // 能进到这里表示数据没有发送完毕
 *      }
 *
 *      // 上面条件不成立，则代表数据发送完毕
 *      r->write_event_handler = ngx_http_request_empty_handler;
 *      ngx_http_finalize_request(r, rc);
 *
 *
 *
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_bufs_t  bufs;
} ngx_http_copy_filter_conf_t;


#if (NGX_HAVE_FILE_AIO)
    static void ngx_http_copy_aio_handler(ngx_output_chain_ctx_t *ctx,
        ngx_file_t *file);
    static void ngx_http_copy_aio_event_handler(ngx_event_t *ev);
    #if (NGX_HAVE_AIO_SENDFILE)
        static ssize_t ngx_http_copy_aio_sendfile_preload(ngx_buf_t *file);
        static void ngx_http_copy_aio_sendfile_event_handler(ngx_event_t *ev);
    #endif
#endif

#if (NGX_THREADS)
    static ngx_int_t ngx_http_copy_thread_handler(ngx_thread_task_t *task,
        ngx_file_t *file);
    static void ngx_http_copy_thread_event_handler(ngx_event_t *ev);
#endif

static void *ngx_http_copy_filter_create_conf(ngx_conf_t *cf);
static char *ngx_http_copy_filter_merge_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_copy_filter_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_copy_filter_commands[] = {

        /*
         * 这个指令只为从硬盘读取数据时候才会用到,也就是说当读文件时不支持sendfile方法才会用到他
         */
    { ngx_string("output_buffers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_copy_filter_conf_t, bufs),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_copy_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_copy_filter_init,             /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_copy_filter_create_conf,      /* create location configuration */
    ngx_http_copy_filter_merge_conf        /* merge location configuration */
};


ngx_module_t  ngx_http_copy_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_copy_filter_module_ctx,      /* module context */
    ngx_http_copy_filter_commands,         /* module directives */
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


static ngx_int_t
ngx_http_copy_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                     rc;
    ngx_connection_t             *c;
    ngx_output_chain_ctx_t       *ctx;
    ngx_http_core_loc_conf_t     *clcf;
    ngx_http_copy_filter_conf_t  *conf;

    c = r->connection;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http copy filter: \"%V?%V\"", &r->uri, &r->args);

    /*
     * 获取ngx_http_copy_filter_module的自定义上下文,如果为空的话后面会创建一个
     */
    ctx = ngx_http_get_module_ctx(r, ngx_http_copy_filter_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_output_chain_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_copy_filter_module);

        conf = ngx_http_get_module_loc_conf(r, ngx_http_copy_filter_module);
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        // 设置sendfile标记,如果操作系统支持并开启了sendfile指令则该值为1
        ctx->sendfile = c->sendfile;

        /*
         * 标记当前处理的数据是否需要在内存中,如果这个标记为1,那么即使支持sendfile,则当前数据也需要
         * 拷贝到内存中才能发送出去
         *
         * 比如/http/modules/ngx_http_gzip_filter_module.c过滤器,需要需要对输出的内容做压缩,
         * 所以需要把文件中的数据拷贝到内存中取处理(r->main_filter_need_in_memory)
         *
         * 比如/http/modules/ngx_http_gunzip_filter_module.c过滤器,因为要对输出的内容做解压,
         * 所以需要把文件中的数据拷贝到内存中取处理(r->filter_need_in_memory)
         *
         * 比如/http/modules/ngx_http_charset_filter_module.c过滤器,需要对输出的内容做编码
         * 转换,所以需要把文件中的数据拷贝到内存中取处理(r->filter_need_in_memory)
         *
         * 所以开启gzip后sendfile就失效了
         */
        ctx->need_in_memory = r->main_filter_need_in_memory
                              || r->filter_need_in_memory;
        /*
         * 类似ctx->need_in_memory,决定数据能否直接发送出去,不能的话就需要拷贝到内存中
         *
         * 模块/http/modules/ngx_http_charset_filter_module.c会用到这个标记,该过滤器用到了两个
         * 标记r->filter_need_temporary和r->filter_need_in_memory
         */
        ctx->need_in_temp = r->filter_need_temporary;

        /*
         * directio要求的对齐值
         */
        ctx->alignment = clcf->directio_alignment;

        ctx->pool = r->pool;
        // 设置读文件用的的缓存个数和大小
        ctx->bufs = conf->bufs;
        ctx->tag = (ngx_buf_tag_t) &ngx_http_copy_filter_module;

        ctx->output_filter = (ngx_output_chain_filter_pt)
                                  ngx_http_next_body_filter;
        ctx->filter_ctx = r;

#if (NGX_HAVE_FILE_AIO)
        if (ngx_file_aio && clcf->aio == NGX_HTTP_AIO_ON) {
            ctx->aio_handler = ngx_http_copy_aio_handler;
#if (NGX_HAVE_AIO_SENDFILE)
            ctx->aio_preload = ngx_http_copy_aio_sendfile_preload;
#endif
        }
#endif

#if (NGX_THREADS)
        if (clcf->aio == NGX_HTTP_AIO_THREADS) {
            ctx->thread_handler = ngx_http_copy_thread_handler;
        }
#endif

        if (in && in->buf && ngx_buf_size(in->buf)) {
            // TODO 干啥的
            r->request_output = 1;
        }
    }

#if (NGX_HAVE_FILE_AIO || NGX_THREADS)
    ctx->aio = r->aio;
#endif

    /*
     * 从这里可以看出cony过滤器对他后面的所有过滤器是一个包围结构,也就是说后面所有的过滤器都返回后,
     * copy过滤器会再设置完r->buffered后才返回给上一个过滤器
     *
     * 过滤器是可以反复执行的,执行的依据是链表in,每个过滤器都会根据当次传入的链表in来做一些判断
     *
     * 这一步才是正真涉及拷贝的方法,说是拷贝实际上是读取数据,文件中的数据读到内存,或者内存中的数据读到内存
     *
     * 在这个方法里，in中的所有buf都会被放到ctx->in中，后续本请求都会围绕ctx->in来输出数据
     */
    rc = ngx_output_chain(ctx, in);

    if (ctx->in == NULL) {
        r->buffered &= ~NGX_HTTP_COPY_BUFFERED;

    } else {
        r->buffered |= NGX_HTTP_COPY_BUFFERED;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http copy filter: %i \"%V?%V\"", rc, &r->uri, &r->args);

    return rc;
}


#if (NGX_HAVE_FILE_AIO)

static void
ngx_http_copy_aio_handler(ngx_output_chain_ctx_t *ctx, ngx_file_t *file)
{
    ngx_http_request_t *r;

    r = ctx->filter_ctx;

    file->aio->data = r;
    file->aio->handler = ngx_http_copy_aio_event_handler;

    r->main->blocked++;
    r->aio = 1;
    ctx->aio = 1;
}


static void
ngx_http_copy_aio_event_handler(ngx_event_t *ev)
{
    ngx_event_aio_t     *aio;
    ngx_http_request_t  *r;

    aio = ev->data;
    r = aio->data;

    r->main->blocked--;
    r->aio = 0;

    r->connection->write->handler(r->connection->write);
}


#if (NGX_HAVE_AIO_SENDFILE)

static ssize_t
ngx_http_copy_aio_sendfile_preload(ngx_buf_t *file)
{
    ssize_t              n;
    static u_char        buf[1];
    ngx_event_aio_t     *aio;
    ngx_http_request_t  *r;

    n = ngx_file_aio_read(file->file, buf, 1, file->file_pos, NULL);

    if (n == NGX_AGAIN) {
        aio = file->file->aio;
        aio->handler = ngx_http_copy_aio_sendfile_event_handler;

        r = aio->data;
        r->main->blocked++;
        r->aio = 1;
    }

    return n;
}


static void
ngx_http_copy_aio_sendfile_event_handler(ngx_event_t *ev)
{
    ngx_event_aio_t     *aio;
    ngx_http_request_t  *r;

    aio = ev->data;
    r = aio->data;

    r->main->blocked--;
    r->aio = 0;
    ev->complete = 0;

    r->connection->write->handler(r->connection->write);
}

#endif
#endif


#if (NGX_THREADS)

static ngx_int_t
ngx_http_copy_thread_handler(ngx_thread_task_t *task, ngx_file_t *file)
{
    ngx_str_t                  name;
    ngx_thread_pool_t         *tp;
    ngx_http_request_t        *r;
    ngx_http_core_loc_conf_t  *clcf;

    r = file->thread_ctx;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    tp = clcf->thread_pool;

    if (tp == NULL) {
        if (ngx_http_complex_value(r, clcf->thread_pool_value, &name)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        tp = ngx_thread_pool_get((ngx_cycle_t *) ngx_cycle, &name);

        if (tp == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "thread pool \"%V\" not found", &name);
            return NGX_ERROR;
        }
    }

    task->event.data = r;
    task->event.handler = ngx_http_copy_thread_event_handler;

    if (ngx_thread_task_post(tp, task) != NGX_OK) {
        return NGX_ERROR;
    }

    r->main->blocked++;
    r->aio = 1;

    return NGX_OK;
}


static void
ngx_http_copy_thread_event_handler(ngx_event_t *ev)
{
    ngx_http_request_t  *r;

    r = ev->data;

    r->main->blocked--;
    r->aio = 0;

    r->connection->write->handler(r->connection->write);
}

#endif


static void *
ngx_http_copy_filter_create_conf(ngx_conf_t *cf)
{
    ngx_http_copy_filter_conf_t *conf;

    conf = ngx_palloc(cf->pool, sizeof(ngx_http_copy_filter_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->bufs.num = 0;

    return conf;
}


static char *
ngx_http_copy_filter_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_copy_filter_conf_t *prev = parent;
    ngx_http_copy_filter_conf_t *conf = child;

    /*
     * 1.9.6及其之后的版本默认是2个buf,每个buf 32k
     */
    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs, 1, 32768);

    return NULL;
}


static ngx_int_t
ngx_http_copy_filter_init(ngx_conf_t *cf)
{
    /*
     * 注册过滤器
     */

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_copy_filter;

    return NGX_OK;
}

