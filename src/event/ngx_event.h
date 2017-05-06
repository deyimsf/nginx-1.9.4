
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_EVENT_H_INCLUDED_
#define _NGX_EVENT_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#define NGX_INVALID_INDEX  0xd0d0d0d0


#if (NGX_HAVE_IOCP)

typedef struct {
    WSAOVERLAPPED    ovlp;
    ngx_event_t     *event;
    int              error;
} ngx_event_ovlp_t;

#endif


/**
 * nginx对事件的抽象
 *
 */
struct ngx_event_s {
    void            *data;

    unsigned         write:1;

    // 标记为1,表示该事件对应的是监听连接
    unsigned         accept:1;

    /* used to detect the stale events in kqueue and epoll */
    /*
     * 用来标记该事件对应的连接(ngx_connection_t)是否新鲜(stale)
     * 也就是该事件对应的连接从第一次放入到epoll中,到该事件被处理之前是否被释放过
     */
    unsigned         instance:1;

    /*
     * the event was passed or would be passed to a kernel;
     * in aio mode - operation was posted.
     */
    // 该事件对应的事件类型存在epoll中
    unsigned         active:1;

    unsigned         disabled:1;

    /* the ready event; in aio mode 0 means that no operation can be posted */
    // 有数据可读
    unsigned         ready:1;

    unsigned         oneshot:1;

    /* aio operation is complete */
    unsigned         complete:1;

    unsigned         eof:1;
    unsigned         error:1;

    unsigned         timedout:1;
    unsigned         timer_set:1;

    unsigned         delayed:1;

    // 是否延迟连接tcp连接,当为1时表示延迟建立tcp连接,只有连接上真正有数据的时候才建立连接
    unsigned         deferred_accept:1;

    /* the pending eof reported by kqueue, epoll or in aio chain operation */
    unsigned         pending_eof:1;

    unsigned         posted:1;

    // TODO
    unsigned         closed:1;

    /* to test on worker exit */
    unsigned         channel:1;
    unsigned         resolver:1;

    unsigned         cancelable:1;

#if (NGX_WIN32)
    /* setsockopt(SO_UPDATE_ACCEPT_CONTEXT) was successful */
    unsigned         accept_context_updated:1;
#endif

#if (NGX_HAVE_KQUEUE)
    unsigned         kq_vnode:1;

    /* the pending errno reported by kqueue */
    int              kq_errno;
#endif

    /*
     * kqueue only:
     *   accept:     number of sockets that wait to be accepted
     *   read:       bytes to read when event is ready
     *               or lowat when event is set with NGX_LOWAT_EVENT flag
     *   write:      available space in buffer when event is ready
     *               or lowat when event is set with NGX_LOWAT_EVENT flag
     *
     * iocp: TODO
     *
     * otherwise:
     *   accept:     1 if accept many, 0 otherwise
     */

#if (NGX_HAVE_KQUEUE) || (NGX_HAVE_IOCP)
    int              available;
#else
    /*
     * 如果该事件是监听连接的接收事件则
     * 表示是否尽量多的建立TCP连接
     * 该值有multi_accept指令负责(on|off)
     */
    unsigned         available:1;
#endif

    // 事件发生后要做的事
    ngx_event_handler_pt  handler;


#if (NGX_HAVE_IOCP)
    ngx_event_ovlp_t ovlp;
#endif

    // TODO
    ngx_uint_t       index;

    ngx_log_t       *log;

    ngx_rbtree_node_t   timer;

    /* the posted queue */
    ngx_queue_t      queue;

#if 0

    /* the threads support */

    /*
     * the event thread context, we store it here
     * if $(CC) does not understand __thread declaration
     * and pthread_getspecific() is too costly
     */

    void            *thr_ctx;

#if (NGX_EVENT_T_PADDING)

    /* event should not cross cache line in SMP */

    uint32_t         padding[NGX_EVENT_T_PADDING];
#endif
#endif
};


#if (NGX_HAVE_FILE_AIO)

struct ngx_event_aio_s {
    void                      *data;
    ngx_event_handler_pt       handler;
    ngx_file_t                *file;

#if (NGX_HAVE_AIO_SENDFILE)
    ssize_t                  (*preload_handler)(ngx_buf_t *file);
#endif

    ngx_fd_t                   fd;

#if (NGX_HAVE_EVENTFD)
    int64_t                    res;
#endif

#if !(NGX_HAVE_EVENTFD) || (NGX_TEST_BUILD_EPOLL)
    ngx_err_t                  err;
    size_t                     nbytes;
#endif

    ngx_aiocb_t                aiocb;
    ngx_event_t                event;
};

#endif


/**
 * 每个事件模块都必须实现这10个抽象方
 */
typedef struct {
    ngx_int_t  (*add)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*del)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);

    ngx_int_t  (*enable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*disable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);

    ngx_int_t  (*add_conn)(ngx_connection_t *c);
    ngx_int_t  (*del_conn)(ngx_connection_t *c, ngx_uint_t flags);

    ngx_int_t  (*notify)(ngx_event_handler_pt handler);

    ngx_int_t  (*process_events)(ngx_cycle_t *cycle, ngx_msec_t timer,
                   ngx_uint_t flags);

    /*
      fork出worker进程之后,在/src/event/ngx_event.c/ngx_event_process_init方法中,
      会调用该方法(actions.init)
    */
    ngx_int_t  (*init)(ngx_cycle_t *cycle, ngx_msec_t timer);
    void       (*done)(ngx_cycle_t *cycle);
} ngx_event_actions_t;


extern ngx_event_actions_t   ngx_event_actions;


/*
 * The event filter requires to read/write the whole data:
 * select, poll, /dev/poll, kqueue, epoll.
 *
 * 对于epoll来说是使用水平触发机制来触发事件
 */
#define NGX_USE_LEVEL_EVENT      0x00000001

/*
 * The event filter is deleted after a notification without an additional
 * syscall: kqueue, epoll.
 */
#define NGX_USE_ONESHOT_EVENT    0x00000002

/*
 * The event filter notifies only the changes and an initial level:
 * kqueue, epoll.
 *
 * 对于epoll来说是使用边缘触发机制来触发事件
 */
#define NGX_USE_CLEAR_EVENT      0x00000004

/*
 * The event filter has kqueue features: the eof flag, errno,
 * available data, etc.
 */
#define NGX_USE_KQUEUE_EVENT     0x00000008

/*
 * The event filter supports low water mark: kqueue's NOTE_LOWAT.
 * kqueue in FreeBSD 4.1-4.2 has no NOTE_LOWAT so we need a separate flag.
 */
#define NGX_USE_LOWAT_EVENT      0x00000010

/*
 * The event filter requires to do i/o operation until EAGAIN: epoll.
 */
#define NGX_USE_GREEDY_EVENT     0x00000020

/*
 * The event filter is epoll.
 */
#define NGX_USE_EPOLL_EVENT      0x00000040

/*
 * Obsolete.
 */
#define NGX_USE_RTSIG_EVENT      0x00000080

/*
 * Obsolete.
 */
#define NGX_USE_AIO_EVENT        0x00000100

/*
 * Need to add socket or handle only once: i/o completion port.
 */
#define NGX_USE_IOCP_EVENT       0x00000200

/*
 * The event filter has no opaque data and requires file descriptors table:
 * poll, /dev/poll.
 */
#define NGX_USE_FD_EVENT         0x00000400

/*
 * The event module handles periodic or absolute timer event by itself:
 * kqueue in FreeBSD 4.4, NetBSD 2.0, and MacOSX 10.4, Solaris 10's event ports.
 */
#define NGX_USE_TIMER_EVENT      0x00000800

/*
 * All event filters on file descriptor are deleted after a notification:
 * Solaris 10's event ports.
 */
#define NGX_USE_EVENTPORT_EVENT  0x00001000

/*
 * The event filter support vnode notifications: kqueue.
 */
#define NGX_USE_VNODE_EVENT      0x00002000


/*
 * The event filter is deleted just before the closing file.
 * Has no meaning for select and poll.
 * kqueue, epoll, eventport:         allows to avoid explicit delete,
 *                                   because filter automatically is deleted
 *                                   on file close,
 *
 * /dev/poll:                        we need to flush POLLREMOVE event
 *                                   before closing file.
 */
#define NGX_CLOSE_EVENT    1

/*
 * disable temporarily event filter, this may avoid locks
 * in kernel malloc()/free(): kqueue.
 */
#define NGX_DISABLE_EVENT  2

/*
 * event must be passed to kernel right now, do not wait until batch processing.
 */
#define NGX_FLUSH_EVENT    4


/* these flags have a meaning only for kqueue */
#define NGX_LOWAT_EVENT    0
#define NGX_VNODE_EVENT    0


#if (NGX_HAVE_EPOLL) && !(NGX_HAVE_EPOLLRDHUP)
#define EPOLLRDHUP         0
#endif


#if (NGX_HAVE_KQUEUE)

#define NGX_READ_EVENT     EVFILT_READ
#define NGX_WRITE_EVENT    EVFILT_WRITE

#undef  NGX_VNODE_EVENT
#define NGX_VNODE_EVENT    EVFILT_VNODE

/*
 * NGX_CLOSE_EVENT, NGX_LOWAT_EVENT, and NGX_FLUSH_EVENT are the module flags
 * and they must not go into a kernel so we need to choose the value
 * that must not interfere with any existent and future kqueue flags.
 * kqueue has such values - EV_FLAG1, EV_EOF, and EV_ERROR:
 * they are reserved and cleared on a kernel entrance.
 */
#undef  NGX_CLOSE_EVENT
#define NGX_CLOSE_EVENT    EV_EOF

#undef  NGX_LOWAT_EVENT
#define NGX_LOWAT_EVENT    EV_FLAG1

#undef  NGX_FLUSH_EVENT
#define NGX_FLUSH_EVENT    EV_ERROR

#define NGX_LEVEL_EVENT    0
#define NGX_ONESHOT_EVENT  EV_ONESHOT
#define NGX_CLEAR_EVENT    EV_CLEAR

#undef  NGX_DISABLE_EVENT
#define NGX_DISABLE_EVENT  EV_DISABLE


#elif (NGX_HAVE_DEVPOLL || NGX_HAVE_EVENTPORT)

#define NGX_READ_EVENT     POLLIN
#define NGX_WRITE_EVENT    POLLOUT

#define NGX_LEVEL_EVENT    0
#define NGX_ONESHOT_EVENT  1


#elif (NGX_HAVE_EPOLL)

// epoll中的读事件
#define NGX_READ_EVENT     (EPOLLIN|EPOLLRDHUP)
// epoll中的写事件
#define NGX_WRITE_EVENT    EPOLLOUT

#define NGX_LEVEL_EVENT    0
#define NGX_CLEAR_EVENT    EPOLLET
#define NGX_ONESHOT_EVENT  0x70000000
#if 0
#define NGX_ONESHOT_EVENT  EPOLLONESHOT
#endif


#elif (NGX_HAVE_POLL)

#define NGX_READ_EVENT     POLLIN
#define NGX_WRITE_EVENT    POLLOUT

#define NGX_LEVEL_EVENT    0
#define NGX_ONESHOT_EVENT  1


#else /* select */

#define NGX_READ_EVENT     0
#define NGX_WRITE_EVENT    1

#define NGX_LEVEL_EVENT    0
#define NGX_ONESHOT_EVENT  1

#endif /* NGX_HAVE_KQUEUE */


#if (NGX_HAVE_IOCP)
#define NGX_IOCP_ACCEPT      0
#define NGX_IOCP_IO          1
#define NGX_IOCP_CONNECT     2
#endif


#ifndef NGX_CLEAR_EVENT
#define NGX_CLEAR_EVENT    0    /* dummy declaration */
#endif


#define ngx_process_events   ngx_event_actions.process_events
#define ngx_done_events      ngx_event_actions.done

#define ngx_add_event        ngx_event_actions.add
#define ngx_del_event        ngx_event_actions.del
#define ngx_add_conn         ngx_event_actions.add_conn
#define ngx_del_conn         ngx_event_actions.del_conn

#define ngx_notify           ngx_event_actions.notify

// 添加一个时间事件
#define ngx_add_timer        ngx_event_add_timer
// 删除一个时间事件
#define ngx_del_timer        ngx_event_del_timer


extern ngx_os_io_t  ngx_io;

#define ngx_recv             ngx_io.recv
#define ngx_recv_chain       ngx_io.recv_chain
#define ngx_udp_recv         ngx_io.udp_recv
#define ngx_send             ngx_io.send
#define ngx_send_chain       ngx_io.send_chain


#define NGX_EVENT_MODULE      0x544E5645  /* "EVNT" */
#define NGX_EVENT_CONF        0x02000000


/* 该结构体用来保存事件模块的配置信息
 *
 * 如 events {
 * 		worker_connections 1024;
 * 		user epoll;
 * 	  }
 *
 */
typedef struct {
	// woker进程支持的连接数,有worker_connections指令指定
    ngx_uint_t    connections;
    // 使用哪种事件模块(epoll、select等),有use指令指定
    ngx_uint_t    use;

    // 是否尽可能多的获取连接(on|off)
    ngx_flag_t    multi_accept;
    /**
     * 是否开启互斥锁,对应指令accept_mutex,默认on; 1.11.3之后默认是off
     * 如果开启,代表使用负载均衡机制
     */
    ngx_flag_t    accept_mutex;

    // 当accept_mutex开启的时候,如果wokre获取不到均衡锁,则指定worker重新接收(accept)连接的一个延迟时间?
    ngx_msec_t    accept_mutex_delay;

    // 当前事件模块名字(epoll、select等)
    u_char       *name;

#if (NGX_DEBUG)
    ngx_array_t   debug_connection;
#endif
} ngx_event_conf_t;


/**
 * 事件模块的上下文定义
 *
 */
typedef struct {
	// 事件模块名字
    ngx_str_t              *name;

    void                 *(*create_conf)(ngx_cycle_t *cycle);
    char                 *(*init_conf)(ngx_cycle_t *cycle, void *conf);

    // 实现该模块必须定义ngx_event_actions_t中规定的10个抽象方法
    ngx_event_actions_t     actions;
} ngx_event_module_t;


extern ngx_atomic_t          *ngx_connection_counter;

extern ngx_atomic_t          *ngx_accept_mutex_ptr;
extern ngx_shmtx_t            ngx_accept_mutex;
extern ngx_uint_t             ngx_use_accept_mutex;
extern ngx_uint_t             ngx_accept_events;
extern ngx_uint_t             ngx_accept_mutex_held;
extern ngx_msec_t             ngx_accept_mutex_delay;
extern ngx_int_t              ngx_accept_disabled;


#if (NGX_STAT_STUB)

extern ngx_atomic_t  *ngx_stat_accepted;
extern ngx_atomic_t  *ngx_stat_handled;
extern ngx_atomic_t  *ngx_stat_requests;
extern ngx_atomic_t  *ngx_stat_active;
extern ngx_atomic_t  *ngx_stat_reading;
extern ngx_atomic_t  *ngx_stat_writing;
extern ngx_atomic_t  *ngx_stat_waiting;

#endif


#define NGX_UPDATE_TIME         1
#define NGX_POST_EVENTS         2


extern sig_atomic_t           ngx_event_timer_alarm;
extern ngx_uint_t             ngx_event_flags;
extern ngx_module_t           ngx_events_module;
extern ngx_module_t           ngx_event_core_module;


#define ngx_event_get_conf(conf_ctx, module)                                  \
             (*(ngx_get_conf(conf_ctx, ngx_events_module))) [module.ctx_index];



void ngx_event_accept(ngx_event_t *ev);
ngx_int_t ngx_trylock_accept_mutex(ngx_cycle_t *cycle);
u_char *ngx_accept_log_error(ngx_log_t *log, u_char *buf, size_t len);


void ngx_process_events_and_timers(ngx_cycle_t *cycle);
ngx_int_t ngx_handle_read_event(ngx_event_t *rev, ngx_uint_t flags);
ngx_int_t ngx_handle_write_event(ngx_event_t *wev, size_t lowat);


#if (NGX_WIN32)
void ngx_event_acceptex(ngx_event_t *ev);
ngx_int_t ngx_event_post_acceptex(ngx_listening_t *ls, ngx_uint_t n);
u_char *ngx_acceptex_log_error(ngx_log_t *log, u_char *buf, size_t len);
#endif


ngx_int_t ngx_send_lowat(ngx_connection_t *c, size_t lowat);


/* used in ngx_log_debugX() */
#define ngx_event_ident(p)  ((ngx_connection_t *) (p))->fd


#include <ngx_event_timer.h>
#include <ngx_event_posted.h>

#if (NGX_WIN32)
#include <ngx_iocp_module.h>
#endif


#endif /* _NGX_EVENT_H_INCLUDED_ */
