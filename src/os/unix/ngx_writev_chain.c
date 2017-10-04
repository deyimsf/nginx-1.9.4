
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
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


/*
 * 该方法不负责发送文件中的数据,如果链表in中有文件数据的映射则返回错误
 *
 * 文件中的数据由/src/os/unix/ngx_linux_sendfile_chain.c/ngx_linux_sendfile()方法负责发送
 *
 * 另外/src/os/unix/ngx_linux_sendfile_chain.c/ngx_linux_sendfile_chain()方法既可以发送
 * 内存数据,也可以发送文件数据;它发送内存数据的方式和ngx_writev_chain()方法类似,最终用ngx_writev()
 * 方法发送出去;发送文件数据就是用ngx_linux_sendfile()方法
 *
 *
 * 该方法将链表in中的数据发送出去,并且返回未发送的链
 * 比如说in链表中有三个链表项,此次调用只把第一个链表项中的数据发送完毕,那么返回的链表就会以第二个链表项作为链头返回
 * 如果此次调用只发送了第一个链表项中的部分数据,那么第一个链表项仍然作为链表头被返回出去,只不过链表项中buf的指针有改动
 */
ngx_chain_t *
ngx_writev_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    ssize_t        n, sent;
    off_t          send, prev_send;
    ngx_chain_t   *cl;
    ngx_event_t   *wev;
    ngx_iovec_t    vec;
    struct iovec   iovs[NGX_IOVS_PREALLOCATE];

    wev = c->write;

    if (!wev->ready) {
        return in;
    }

#if (NGX_HAVE_KQUEUE)

    if ((ngx_event_flags & NGX_USE_KQUEUE_EVENT) && wev->pending_eof) {
        (void) ngx_connection_error(c, wev->kq_errno,
                               "kevent() reported about an closed connection");
        wev->error = 1;
        return NGX_CHAIN_ERROR;
    }

#endif

    /* the maximum limit size is the maximum size_t value - the page size */

    if (limit == 0 || limit > (off_t) (NGX_MAX_SIZE_T_VALUE - ngx_pagesize)) {
        limit = NGX_MAX_SIZE_T_VALUE - ngx_pagesize;
    }

    send = 0;

    vec.iovs = iovs;
    vec.nalloc = NGX_IOVS_PREALLOCATE;

    for ( ;; ) {
        prev_send = send;

        /* create the iovec and coalesce the neighbouring bufs */
        /* 创建iovec并且内存合并相邻的buf*/

        cl = ngx_output_chain_to_iovec(&vec, in, limit - send, c->log);

        if (cl == NGX_CHAIN_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        /*
         * 该方法不处理文件中的数据,所以如果有这样的数据则直接返回错误
         *
         * TODO 文件中的数据谁处理
         */
        if (cl && cl->buf->in_file) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "file buf in writev "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();

            return NGX_CHAIN_ERROR;
        }

        send += vec.size;

        n = ngx_writev(c, &vec);

        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        sent = (n == NGX_AGAIN) ? 0 : n;

        c->sent += sent;

        in = ngx_chain_update_sent(in, sent);

        // 不相等就代表这次没发送完
        if (send - prev_send != sent) {
        	// TODO
            wev->ready = 0;
            return in;
        }

        if (send >= limit || in == NULL) {
            return in;
        }
    }
}


/*
 * struct iovec {
 *       void *iov_base; // Starting address  开始地址
 *       size_t iov_len; // Number of bytes to transfer 数据长度
 * }
 *
 * ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
 *
 * 将链表in中的数据映射到vec->iovs数组中,准备用writev()方法发送出去
 *
 * limit表示这次最多映射的字节数
 *
 * 该方法不映射在文件中的数据,也就是说不映射buf->in_file中的数据
 *
 * 对于链表in中内存相邻的buf会合并到同一个struct iovec中
 *
 */
ngx_chain_t *
ngx_output_chain_to_iovec(ngx_iovec_t *vec, ngx_chain_t *in, size_t limit,
    ngx_log_t *log)
{
    size_t         total, size;
    u_char        *prev;
    ngx_uint_t     n;
    // 系统方法writev()用到的缓冲区
    struct iovec  *iov;

    iov = NULL;
    prev = NULL;
    // 映射的总字节数
    total = 0;
    // 映射到的链表中链表项的个数
    n = 0;

    for ( /* void */ ; in && total < limit; in = in->next) {

    	/*
    	 * #define ngx_buf_special(b) ((b->flush || b->last_buf || b->sync) && !ngx_buf_in_memory(b) && !b->in_file)
    	 *
    	 * 特殊buf不做映射,也就是说链表中的特殊缓存发送数据的方式和正常的不同
    	 */
        if (ngx_buf_special(in->buf)) {
            continue;
        }

        // 不处理文件中的数据
        if (in->buf->in_file) {
            break;
        }

        // 只处理在内存中的数据
        if (!ngx_buf_in_memory(in->buf)) {
            ngx_log_error(NGX_LOG_ALERT, log, 0,
                          "bad buf in output chain "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          in->buf->temporary,
                          in->buf->recycled,
                          in->buf->in_file,
                          in->buf->start,
                          in->buf->pos,
                          in->buf->last,
                          in->buf->file,
                          in->buf->file_pos,
                          in->buf->file_last);

            ngx_debug_point();

            return NGX_CHAIN_ERROR;
        }

        // 计算出当前buf中的数据
        size = in->buf->last - in->buf->pos;

        /*
         * 如果当前size已经大于当前限制的大小,则将要映射的大小该为当前限制的大小
         */
        if (size > limit - total) {
            size = limit - total;
        }

        /*
         * prev代表当前映射到的地址
         *
         * 如果prev等于当前buf的首地址,那么说明上一个链表中的buf和当前链表中的buf它们在内存中的地址是连续的
         * 如果内存是连续的,那么就不会占用多余的struct iovec
         */
        if (prev == in->buf->pos) {
        	// 内存是连续的所以,映射的内存直接加size个
            iov->iov_len += size;

        } else {
        	// 最多一次允许映射nalloc个
            if (n == vec->nalloc) {
                break;
            }

            // 取出一个struct iovec结构体用来映射链表项
            iov = &vec->iovs[n++];

            // 映射的内存首地址
            iov->iov_base = (void *) in->buf->pos;
            // 映射的内存大小
            iov->iov_len = size;
        }

        // 记录此次映射到的内存的最后地址
        prev = in->buf->pos + size;
        // 累计此次映射的内存大小
        total += size;
    }

    /* 记录映射的数据信息 */
    vec->count = n;
    vec->size = total;

    return in;
}


/*
 * 将vec中的数据发送出去,并返回发送的字节大小
 */
ssize_t
ngx_writev(ngx_connection_t *c, ngx_iovec_t *vec)
{
    ssize_t    n;
    ngx_err_t  err;

eintr:

    n = writev(c->fd, vec->iovs, vec->count);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "writev: %z of %uz", n, vec->size);

    if (n == -1) {
        err = ngx_errno;

        switch (err) {
        case NGX_EAGAIN:
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "writev() not ready");
            return NGX_AGAIN;

        case NGX_EINTR:
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "writev() was interrupted");
            goto eintr;

        default:
            c->write->error = 1;
            ngx_connection_error(c, err, "writev() failed");
            return NGX_ERROR;
        }
    }

    return n;
}
