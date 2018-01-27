
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


static ngx_int_t ngx_enable_accept_events(ngx_cycle_t *cycle);
static ngx_int_t ngx_disable_accept_events(ngx_cycle_t *cycle, ngx_uint_t all);
static void ngx_close_accepted_connection(ngx_connection_t *c);


/**
 * 直接调用accept方法接收新连接
 */
void
ngx_event_accept(ngx_event_t *ev)
{
    socklen_t          socklen;
    ngx_err_t          err;
    ngx_log_t         *log;
    ngx_uint_t         level;
    ngx_socket_t       s;
    ngx_event_t       *rev, *wev;
    ngx_listening_t   *ls;
    ngx_connection_t  *c, *lc;
    ngx_event_conf_t  *ecf;
    // 用数组定义,只是为了在栈上分配内存
    u_char             sa[NGX_SOCKADDRLEN];
#if (NGX_HAVE_ACCEPT4)
    static ngx_uint_t  use_accept4 = 1;
#endif

    // TODO 这个超时是怎么产生的
    if (ev->timedout) {
    	// TODO 超时之后为什么要再一次把监听连接放入到epoll中
        if (ngx_enable_accept_events((ngx_cycle_t *) ngx_cycle) != NGX_OK) {
            return;
        }

        ev->timedout = 0;
    }

    // 获取事件核心模块的配置信息结构体
    ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);

    if (!(ngx_event_flags & NGX_USE_KQUEUE_EVENT)) {
    	// 设置本次是否可以尽可能多的获取新连接
        ev->available = ecf->multi_accept;
    }

    /*
     * 获取连接对象
     * 连接对象中的listening字段就是监听对象
     */
    lc = ev->data;
    /*
     * 获取监听连接对象
     * ls->connection就是上面的lc
     */
    ls = lc->listening;

    // TODO 在这里置0是什么意思
    ev->ready = 0;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "accept on %V, ready: %d", &ls->addr_text, ev->available);

    do {
        socklen = NGX_SOCKADDRLEN;

#if (NGX_HAVE_ACCEPT4)
        if (use_accept4) {
        	/*
        	 * 从监听连接lc->fd中建立一个连接
        	 *
        	 */
            s = accept4(lc->fd, (struct sockaddr *) sa, &socklen,
                        SOCK_NONBLOCK);
        } else {
            s = accept(lc->fd, (struct sockaddr *) sa, &socklen);
        }
#else
        // 从监听连接lc->fd中建立一个连接
        s = accept(lc->fd, (struct sockaddr *) sa, &socklen);
#endif

        /* 调用accept方法时发生错误 */
        if (s == (ngx_socket_t) -1) {
            err = ngx_socket_errno;

            /* non-blocking and interrupt i/o */
            /* Resource temporarily unavailable */
            // 当监听描述符设置为非阻塞时,如果没有可用给的连接,则accept方法直接返回-1,并且值errno为EAGAIN
            if (err == NGX_EAGAIN) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, err,
                               "accept() not ready");

                // 事件没有准备好直接返回。
                return;
            }

            level = NGX_LOG_ALERT;

            if (err == NGX_ECONNABORTED) {
            	// "客户端"连接异常终止
                level = NGX_LOG_ERR;

            } else if (err == NGX_EMFILE || err == NGX_ENFILE) {
                level = NGX_LOG_CRIT;
            }

#if (NGX_HAVE_ACCEPT4)
            ngx_log_error(level, ev->log, err,
                          use_accept4 ? "accept4() failed" : "accept() failed");

            if (use_accept4 && err == NGX_ENOSYS) {
                use_accept4 = 0;
                ngx_inherited_nonblocking = 0;
                continue;
            }
#else
            ngx_log_error(level, ev->log, err, "accept() failed");
#endif

            if (err == NGX_ECONNABORTED) {
                if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
                	// 如果使用KQUEUE事件模块则执行这个操作
                    ev->available--;
                }

                if (ev->available) {
                    continue;
                }
            }

            if (err == NGX_EMFILE || err == NGX_ENFILE) {
            	// 打开的文件描述符太多了,不能再建立新连接了,所以将监听连接事件从epoll中删除掉
                if (ngx_disable_accept_events((ngx_cycle_t *) ngx_cycle, 1)
                    != NGX_OK)
                {
                	// 没有删除成功则直接返回
                    return;
                }

                /* 从epoll中删除监听连接事件成功 */

                if (ngx_use_accept_mutex) {
                    if (ngx_accept_mutex_held) {
                    	// 如果使用互斥锁了,并且当前进程持有锁,则释放锁
                        ngx_shmtx_unlock(&ngx_accept_mutex);
                        ngx_accept_mutex_held = 0;
                    }

                    /*
                     * 负载均衡阀值设置为1,则下次再进入ngx_process_events_and_timers方法后不会去尝试获取锁,
                     * 这样能让当前worker暂时不处理新连接。
                     */
                    ngx_accept_disabled = 1;

                } else {
                	/*
                	 * 如果不是因为占用文件描述符过多造成的问题,则有可能是客户端连接暂时没有准备好,则把该事件
                	 * 放入到时间事件驱动器中,并设置执行时间accept_mutex_delay
                	 */
                    ngx_add_timer(ev, ecf->accept_mutex_delay);
                }
            }

            return;
        }

        /* 调用accept方法时没有发生错误 */

#if (NGX_STAT_STUB)
        // 如果开启了统计模块,则ngx_stat_accepted加1,表示新创建了一个连接
        (void) ngx_atomic_fetch_add(ngx_stat_accepted, 1);
#endif

        // 设置负载均衡开启的阀值,这是一个动态值,每建立一个新连接就更新一次
        ngx_accept_disabled = ngx_cycle->connection_n / 8
                              - ngx_cycle->free_connection_n;

        // 为新建立的网络描述符s分配一个连接对象(ngx_connection_t)
        c = ngx_get_connection(s, ev->log);

        // 没有多余的连接对象(ngx_connection_t)使用了,则直接关闭这个新建立的描述符
        if (c == NULL) {
            if (ngx_close_socket(s) == -1) {
                ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                              ngx_close_socket_n " failed");
            }

            return;
        }

#if (NGX_STAT_STUB)
        // 如果开启了统计模块,则ngx_stat_active加1,表示"客户端"在ngx中关联上了一个连接对象(ngx_connection_t)
        (void) ngx_atomic_fetch_add(ngx_stat_active, 1);
#endif

        // 为该连接创建一个连接池
        c->pool = ngx_create_pool(ls->pool_size, ev->log);
        if (c->pool == NULL) {
        	// 失败则关闭该网络连接
            ngx_close_accepted_connection(c);
            return;
        }

        // 为"客户端"网络描述符s分配内核socket地址空间
        c->sockaddr = ngx_palloc(c->pool, socklen);
        if (c->sockaddr == NULL) {
        	// 失败则关闭该网络连接
            ngx_close_accepted_connection(c);
            return;
        }

        // 将网络描述符s的内核socket数据,copy到c->sockadd中
        ngx_memcpy(c->sockaddr, sa, socklen);

        // 为该连接创建一个日志对象
        log = ngx_palloc(c->pool, sizeof(ngx_log_t));
        if (log == NULL) {
        	// 失败则关闭该网络连接
            ngx_close_accepted_connection(c);
            return;
        }

        /* set a blocking mode for iocp and non-blocking mode for others */

        if (ngx_inherited_nonblocking) {
            if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
                if (ngx_blocking(s) == -1) {
                    ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                                  ngx_blocking_n " failed");
                    ngx_close_accepted_connection(c);
                    return;
                }
            }

        } else {
            if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) {
            	// socket设置成非阻塞
                if (ngx_nonblocking(s) == -1) {
                    ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                                  ngx_nonblocking_n " failed");
                    ngx_close_accepted_connection(c);
                    return;
                }
            }
        }

        *log = ls->log;

        // 该连接上的读方法
        c->recv = ngx_recv;
        // 写方法
        c->send = ngx_send;

        // 在/src/event/ngx_event_pipe.c/ngx_event_pipe_read_upstream方法中有调用
        c->recv_chain = ngx_recv_chain;
        /*
         * 在下面三个文件中有调用
         * /src/core/ngx_output_chain.c
         * /src/http/ngx_http_spdy.c
         * /src/http/ngx_http_write_filter_module.c
         */
        c->send_chain = ngx_send_chain;

        c->log = log;
        c->pool->log = log;

        c->socklen = socklen;
        c->listening = ls;
        c->local_sockaddr = ls->sockaddr;
        c->local_socklen = ls->socklen;

        // TODO干啥的
        c->unexpected_eof = 1;

#if (NGX_HAVE_UNIX_DOMAIN)
        if (c->sockaddr->sa_family == AF_UNIX) {
            c->tcp_nopush = NGX_TCP_NOPUSH_DISABLED;
            c->tcp_nodelay = NGX_TCP_NODELAY_DISABLED;
#if (NGX_SOLARIS)
            /* Solaris's sendfilev() supports AF_NCA, AF_INET, and AF_INET6 */
            c->sendfile = 0;
#endif
        }
#endif

        rev = c->read;
        wev = c->write;

        wev->ready = 1;

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
            rev->ready = 1;
        }

        if (ev->deferred_accept) {
        	/* 这段逻辑啥意思 TODO */

            rev->ready = 1;
#if (NGX_HAVE_KQUEUE)
            rev->available = 1;
#endif
        }

        rev->log = log;
        wev->log = log;

        /*
         * TODO: MT: - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         *
         * TODO: MP: - allocated in a shared memory
         *           - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         */

        // 当前连接是第多少个连接,也可以表示从ngx启动到现在总共接收了多少个tcp
        c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_handled, 1);
#endif

        if (ls->addr_ntop) {
        	// 将内核socket对应的ip地址,转换成字符串形式地址

            c->addr_text.data = ngx_pnalloc(c->pool, ls->addr_text_max_len);
            if (c->addr_text.data == NULL) {
                ngx_close_accepted_connection(c);
                return;
            }

            c->addr_text.len = ngx_sock_ntop(c->sockaddr, c->socklen,
                                             c->addr_text.data,
                                             ls->addr_text_max_len, 0);
            if (c->addr_text.len == 0) {
                ngx_close_accepted_connection(c);
                return;
            }
        }

#if (NGX_DEBUG)
        {

        ngx_str_t             addr;
        struct sockaddr_in   *sin;
        ngx_cidr_t           *cidr;
        ngx_uint_t            i;
        u_char                text[NGX_SOCKADDR_STRLEN];
#if (NGX_HAVE_INET6)
        struct sockaddr_in6  *sin6;
        ngx_uint_t            n;
#endif

        cidr = ecf->debug_connection.elts;
        for (i = 0; i < ecf->debug_connection.nelts; i++) {
            if (cidr[i].family != (ngx_uint_t) c->sockaddr->sa_family) {
                goto next;
            }

            switch (cidr[i].family) {

#if (NGX_HAVE_INET6)
            case AF_INET6:
                sin6 = (struct sockaddr_in6 *) c->sockaddr;
                for (n = 0; n < 16; n++) {
                    if ((sin6->sin6_addr.s6_addr[n]
                        & cidr[i].u.in6.mask.s6_addr[n])
                        != cidr[i].u.in6.addr.s6_addr[n])
                    {
                        goto next;
                    }
                }
                break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
            case AF_UNIX:
                break;
#endif

            default: /* AF_INET */
                sin = (struct sockaddr_in *) c->sockaddr;
                if ((sin->sin_addr.s_addr & cidr[i].u.in.mask)
                    != cidr[i].u.in.addr)
                {
                    goto next;
                }
                break;
            }

            log->log_level = NGX_LOG_DEBUG_CONNECTION|NGX_LOG_DEBUG_ALL;
            break;

        next:
            continue;
        }

        if (log->log_level & NGX_LOG_DEBUG_EVENT) {
            addr.data = text;
            addr.len = ngx_sock_ntop(c->sockaddr, c->socklen, text,
                                     NGX_SOCKADDR_STRLEN, 1);

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, log, 0,
                           "*%uA accept: %V fd:%d", c->number, &addr, s);
        }

        }
#endif

        if (ngx_add_conn && (ngx_event_flags & NGX_USE_EPOLL_EVENT) == 0) {
            if (ngx_add_conn(c) == NGX_ERROR) {
                ngx_close_accepted_connection(c);
                return;
            }
        }

        log->data = NULL;
        log->handler = NULL;

        // 接收完一个新链接后调用该方法
        // 这个方法有各个模块自己设置
        // 比如http核心模块可以把自己的入口方法注入到监听的ls(ngx_listening_t)中
        // http模块用方法ngx_http_add_listening,把ls->handler = ngx_http_init_connection;
        ls->handler(c);

        if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
        	// KQUEUE模块使用,epoll没用
            ev->available--;
        }

       /*
        * ev->available标志位为1时，表示尽量多的建立TCP连接,该值有multi_accept指令负责
        * 当accept方法产生EAGAIN错误时会终止该循环
        */
    } while (ev->available);
}


/**
 * 试着去获取互斥锁,不管有没有获取到锁,都会返回NGX_OK
 * 如果发生错误则返回NGX_ERROR
 */
ngx_int_t
ngx_trylock_accept_mutex(ngx_cycle_t *cycle)
{
    if (ngx_shmtx_trylock(&ngx_accept_mutex)) {

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "accept mutex locked");

        /*
         * 如果ngx_accept_mutex_held已经是1了,则直接返回OK;
         * ngx_accept_events是ngx_eventport_module事件模块用到的
         */
        if (ngx_accept_mutex_held && ngx_accept_events == 0) {
            return NGX_OK;
        }


        // 获取锁之后就把该worker中的所有监听连接都放入到事件驱动器中
        if (ngx_enable_accept_events(cycle) == NGX_ERROR) {
        	// 添加事件是失败则释放锁
            ngx_shmtx_unlock(&ngx_accept_mutex);
            return NGX_ERROR;
        }

        ngx_accept_events = 0;

        // 获取互斥锁
        ngx_accept_mutex_held = 1;

        return NGX_OK;
    }

    // 走到这里表示没有获取到锁
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "accept mutex lock failed: %ui", ngx_accept_mutex_held);

    /*
     * 如果没有获取到锁,但是ngx_accept_mutex_held标志位且是1,那么说明该worker曾经获取到过锁
     * (因为woker在处理完ngx_posted_accept_events队列里的事件并释放锁后,并没有将ngx_accept_mutex_held变量置为零)
     *
     * 所以调用方法ngx_disable_accept_events,将事件驱动器中的监听连接事件删除掉。
     */
    if (ngx_accept_mutex_held) {
        if (ngx_disable_accept_events(cycle, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }

        // 将该值设置为零,表示该进程没有获取到锁,并且epoll中也不存在监听连接读事件了。
        ngx_accept_mutex_held = 0;
    }

    return NGX_OK;
}


/**
 * 将cycle中的所有监听连接放入到事件驱动器中
 */
static ngx_int_t
ngx_enable_accept_events(ngx_cycle_t *cycle)
{
    ngx_uint_t         i;
    ngx_listening_t   *ls;
    ngx_connection_t  *c;

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

        c = ls[i].connection;

        if (c == NULL || c->read->active) {
            continue;
        }

        if (ngx_add_event(c->read, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


/**
 * 将cycle中的所有监听连接从事件驱动器中删除
 */
static ngx_int_t
ngx_disable_accept_events(ngx_cycle_t *cycle, ngx_uint_t all)
{
    ngx_uint_t         i;
    ngx_listening_t   *ls;
    ngx_connection_t  *c;

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

        c = ls[i].connection;

        if (c == NULL || !c->read->active) {
            continue;
        }

#if (NGX_HAVE_REUSEPORT)

        /*
         * do not disable accept on worker's own sockets
         * when disabling accept events due to accept mutex
         */

        if (ls[i].reuseport && !all) {
            continue;
        }

#endif

        if (ngx_del_event(c->read, NGX_READ_EVENT, NGX_DISABLE_EVENT)
            == NGX_ERROR)
        {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_close_accepted_connection(ngx_connection_t *c)
{
    ngx_socket_t  fd;

    ngx_free_connection(c);

    fd = c->fd;
    c->fd = (ngx_socket_t) -1;

    if (ngx_close_socket(fd) == -1) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_socket_errno,
                      ngx_close_socket_n " failed");
    }

    if (c->pool) {
        ngx_destroy_pool(c->pool);
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, -1);
#endif
}


u_char *
ngx_accept_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    return ngx_snprintf(buf, len, " while accepting new connection on %V",
                        log->data);
}
