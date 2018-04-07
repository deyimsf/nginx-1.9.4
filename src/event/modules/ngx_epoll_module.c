
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#if (NGX_TEST_BUILD_EPOLL)

/* epoll declarations */

#define EPOLLIN        0x001
#define EPOLLPRI       0x002
#define EPOLLOUT       0x004
#define EPOLLRDNORM    0x040
#define EPOLLRDBAND    0x080
#define EPOLLWRNORM    0x100
#define EPOLLWRBAND    0x200
#define EPOLLMSG       0x400
#define EPOLLERR       0x008
#define EPOLLHUP       0x010

#define EPOLLRDHUP     0x2000

#define EPOLLET        0x80000000
#define EPOLLONESHOT   0x40000000

#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_DEL  2
#define EPOLL_CTL_MOD  3

typedef union epoll_data {
    void         *ptr;
    int           fd;
    uint32_t      u32;
    uint64_t      u64;
} epoll_data_t;

struct epoll_event {
    uint32_t      events;
    epoll_data_t  data;
};


int epoll_create(int size);

int epoll_create(int size)
{
    return -1;
}


int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    return -1;
}


int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout);

int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout)
{
    return -1;
}

#if (NGX_HAVE_EVENTFD)
#define SYS_eventfd       323
#endif

#if (NGX_HAVE_FILE_AIO)

#define SYS_io_setup      245
#define SYS_io_destroy    246
#define SYS_io_getevents  247

typedef u_int  aio_context_t;

struct io_event {
    uint64_t  data;  /* the data field from the iocb */
    uint64_t  obj;   /* what iocb this event came from */
    int64_t   res;   /* result code for this event */
    int64_t   res2;  /* secondary result */
};


#endif
#endif /* NGX_TEST_BUILD_EPOLL */


/**
 * 用来存放epoll事件模块配置信息的结构体
 */
typedef struct {
    /*
     * 每次调用epoll_wait时可以返回的最大就绪事件个数,对应epoll_events指令(官方文档没有标出这个指令)
     * 默认是512
     */
    ngx_uint_t  events;
    // 对应worker_aio_requests指令
    ngx_uint_t  aio_requests;
} ngx_epoll_conf_t;


static ngx_int_t ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer);
#if (NGX_HAVE_EVENTFD)
static ngx_int_t ngx_epoll_notify_init(ngx_log_t *log);
static void ngx_epoll_notify_handler(ngx_event_t *ev);
#endif
static void ngx_epoll_done(ngx_cycle_t *cycle);
static ngx_int_t ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_epoll_add_connection(ngx_connection_t *c);
static ngx_int_t ngx_epoll_del_connection(ngx_connection_t *c,
    ngx_uint_t flags);
#if (NGX_HAVE_EVENTFD)
static ngx_int_t ngx_epoll_notify(ngx_event_handler_pt handler);
#endif
static ngx_int_t ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags);

#if (NGX_HAVE_FILE_AIO)
static void ngx_epoll_eventfd_handler(ngx_event_t *ev);
#endif

static void *ngx_epoll_create_conf(ngx_cycle_t *cycle);
static char *ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf);

static int                  ep = -1;
static struct epoll_event  *event_list;
static ngx_uint_t           nevents;

#if (NGX_HAVE_EVENTFD)
static int                  notify_fd = -1;
static ngx_event_t          notify_event;
static ngx_connection_t     notify_conn;
#endif

#if (NGX_HAVE_FILE_AIO)

int                         ngx_eventfd = -1;
aio_context_t               ngx_aio_ctx = 0;

static ngx_event_t          ngx_eventfd_event;
static ngx_connection_t     ngx_eventfd_conn;

#endif

static ngx_str_t      epoll_name = ngx_string("epoll");

static ngx_command_t  ngx_epoll_commands[] = {

    // 每次调用epoll_wait时可以返回的最大就绪事件个数
    { ngx_string("epoll_events"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_epoll_conf_t, events),
      NULL },

    { ngx_string("worker_aio_requests"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_epoll_conf_t, aio_requests),
      NULL },

      ngx_null_command
};


ngx_event_module_t  ngx_epoll_module_ctx = {
    &epoll_name,
    ngx_epoll_create_conf,               /* create configuration */
    ngx_epoll_init_conf,                 /* init configuration */

    {
        ngx_epoll_add_event,             /* add an event */
        ngx_epoll_del_event,             /* delete an event */
        ngx_epoll_add_event,             /* enable an event */
        ngx_epoll_del_event,             /* disable an event */
        ngx_epoll_add_connection,        /* add an connection */
        ngx_epoll_del_connection,        /* delete an connection */
#if (NGX_HAVE_EVENTFD)
        ngx_epoll_notify,                /* trigger a notify */
#else
        NULL,                            /* trigger a notify */
#endif
        ngx_epoll_process_events,        /* process the events */

       /*
        *  fork出worker进程之后,在/src/event/ngx_event.c/ngx_event_process_init方法中,
        * 会调用该方法(actions.init)
        */
        ngx_epoll_init,                  /* init the events */

        // 销毁epoll对象,目前没有模块在使用这个方法
        ngx_epoll_done,                  /* done the events */
    }
};


/**
 * 声明一个ngx模块,这个模块是NGX_EVENT_MODULE类型的
 * 该类型的模块需要围绕ngx_event_module_t对象的配置来做事
 *
 * 如果非要和面向对象编程比较的话,ngx_module_t更像一个超类,ngx_event_module_t就想一个子类
 * ngx中所有的操作都围绕着ngx_module_t中的协定来做事,像ngx_core_module_t(核心模块类型)、ngx_event_module_t(事件模块类型)
 * ,这些需要更具体的模块,他们又可以携带一些信息,来规定更具体的操作。
 */
ngx_module_t  ngx_epoll_module = {
    NGX_MODULE_V1,
    &ngx_epoll_module_ctx,               /* module context */
    ngx_epoll_commands,                  /* module directives */
    NGX_EVENT_MODULE,                    /* module type */
    NULL,                                /* init master */
    NULL,                                /* init module */
    NULL,                                /* init process */
    NULL,                                /* init thread */
    NULL,                                /* exit thread */
    NULL,                                /* exit process */
    NULL,                                /* exit master */
    NGX_MODULE_V1_PADDING
};


#if (NGX_HAVE_FILE_AIO)

/*
 * We call io_setup(), io_destroy() io_submit(), and io_getevents() directly
 * as syscalls instead of libaio usage, because the library header file
 * supports eventfd() since 0.3.107 version only.
 */

static int
io_setup(u_int nr_reqs, aio_context_t *ctx)
{
    return syscall(SYS_io_setup, nr_reqs, ctx);
}


static int
io_destroy(aio_context_t ctx)
{
    return syscall(SYS_io_destroy, ctx);
}


static int
io_getevents(aio_context_t ctx, long min_nr, long nr, struct io_event *events,
    struct timespec *tmo)
{
    return syscall(SYS_io_getevents, ctx, min_nr, nr, events, tmo);
}


static void
ngx_epoll_aio_init(ngx_cycle_t *cycle, ngx_epoll_conf_t *epcf)
{
    int                 n;
    struct epoll_event  ee;

#if (NGX_HAVE_SYS_EVENTFD_H)
    ngx_eventfd = eventfd(0, 0);
#else
    ngx_eventfd = syscall(SYS_eventfd, 0);
#endif

    if (ngx_eventfd == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "eventfd() failed");
        ngx_file_aio = 0;
        return;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "eventfd: %d", ngx_eventfd);

    n = 1;

    if (ioctl(ngx_eventfd, FIONBIO, &n) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "ioctl(eventfd, FIONBIO) failed");
        goto failed;
    }

    if (io_setup(epcf->aio_requests, &ngx_aio_ctx) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "io_setup() failed");
        goto failed;
    }

    ngx_eventfd_event.data = &ngx_eventfd_conn;
    ngx_eventfd_event.handler = ngx_epoll_eventfd_handler;
    ngx_eventfd_event.log = cycle->log;
    ngx_eventfd_event.active = 1;
    ngx_eventfd_conn.fd = ngx_eventfd;
    ngx_eventfd_conn.read = &ngx_eventfd_event;
    ngx_eventfd_conn.log = cycle->log;

    ee.events = EPOLLIN|EPOLLET;
    ee.data.ptr = &ngx_eventfd_conn;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, ngx_eventfd, &ee) != -1) {
        return;
    }

    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                  "epoll_ctl(EPOLL_CTL_ADD, eventfd) failed");

    if (io_destroy(ngx_aio_ctx) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "io_destroy() failed");
    }

failed:

    if (close(ngx_eventfd) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "eventfd close() failed");
    }

    ngx_eventfd = -1;
    ngx_aio_ctx = 0;
    ngx_file_aio = 0;
}

#endif


/*
  fork出worker进程之后,在/src/event/ngx_event.c/ngx_event_process_init方法中,
  会调用该方法(actions.init)。

  该方法做了下面这几件事:
  1.创建epoll对象
  2.创建用于接收就绪事件的epoll_event数组
  3.绑定ngx_event_actions变量,该变量中的方法用于操作事件驱动器中的事件
*/
static ngx_int_t
ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_epoll_conf_t  *epcf;

    // 获取eopll事件模块的配置信息
    epcf = ngx_event_get_conf(cycle->conf_ctx, ngx_epoll_module);

    // 调用epoll_create创建epoll对象
    if (ep == -1) {
        ep = epoll_create(cycle->connection_n / 2);

        if (ep == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "epoll_create() failed");
            return NGX_ERROR;
        }

#if (NGX_HAVE_EVENTFD)
        if (ngx_epoll_notify_init(cycle->log) != NGX_OK) {
            ngx_epoll_module_ctx.actions.notify = NULL;
        }
#endif

#if (NGX_HAVE_FILE_AIO)

        ngx_epoll_aio_init(cycle, epcf);

#endif
    }

    // 如果当前用于接收事件数组的长度,小于指定的长度,则重新设置长度并重新创建数组
    if (nevents < epcf->events) {
        if (event_list) {
            ngx_free(event_list);
        }

        // 创建用于从epoll_wait方法中返回的事件数组
        event_list = ngx_alloc(sizeof(struct epoll_event) * epcf->events,
                               cycle->log);
        if (event_list == NULL) {
            return NGX_ERROR;
        }
    }

    nevents = epcf->events;

    ngx_io = ngx_os_io;

    /* 这是一个全局变量,在/src/event/ngx_event.h中通过extern关键字声明出去
     * 其它模块可以使用ngx_event_actions变量中的事件方法,向事件驱动器(epoll、kqueue等)中操作关心的事件
     * 该操作表示使用epoll驱动器提供的方法
     */
    ngx_event_actions = ngx_epoll_module_ctx.actions;

    // 为ngx_event_flags打标,这些NGX_USE_开头的标记都是啥意思呢
#if (NGX_HAVE_CLEAR_EVENT)
    ngx_event_flags = NGX_USE_CLEAR_EVENT
#else
    ngx_event_flags = NGX_USE_LEVEL_EVENT
#endif
                      |NGX_USE_GREEDY_EVENT
                      |NGX_USE_EPOLL_EVENT;

    return NGX_OK;
}


#if (NGX_HAVE_EVENTFD)

static ngx_int_t
ngx_epoll_notify_init(ngx_log_t *log)
{
    struct epoll_event  ee;

#if (NGX_HAVE_SYS_EVENTFD_H)
    notify_fd = eventfd(0, 0);
#else
    notify_fd = syscall(SYS_eventfd, 0);
#endif

    if (notify_fd == -1) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "eventfd() failed");
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "notify eventfd: %d", notify_fd);

    notify_event.handler = ngx_epoll_notify_handler;
    notify_event.log = log;
    notify_event.active = 1;

    notify_conn.fd = notify_fd;
    notify_conn.read = &notify_event;
    notify_conn.log = log;

    ee.events = EPOLLIN|EPOLLET;
    ee.data.ptr = &notify_conn;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, notify_fd, &ee) == -1) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "epoll_ctl(EPOLL_CTL_ADD, eventfd) failed");

        if (close(notify_fd) == -1) {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                            "eventfd close() failed");
        }

        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_epoll_notify_handler(ngx_event_t *ev)
{
    ssize_t               n;
    uint64_t              count;
    ngx_err_t             err;
    ngx_event_handler_pt  handler;

    if (++ev->index == NGX_MAX_UINT32_VALUE) {
        ev->index = 0;

        n = read(notify_fd, &count, sizeof(uint64_t));

        err = ngx_errno;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "read() eventfd %d: %z count:%uL", notify_fd, n, count);

        if ((size_t) n != sizeof(uint64_t)) {
            ngx_log_error(NGX_LOG_ALERT, ev->log, err,
                          "read() eventfd %d failed", notify_fd);
        }
    }

    handler = ev->data;
    handler(ev);
}

#endif


/**
 * 关闭(close)epoll对象
 */
static void
ngx_epoll_done(ngx_cycle_t *cycle)
{
    // 关闭epoll对象
    if (close(ep) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "epoll close() failed");
    }

    ep = -1;

#if (NGX_HAVE_EVENTFD)

    if (close(notify_fd) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "eventfd close() failed");
    }

    notify_fd = -1;

#endif

#if (NGX_HAVE_FILE_AIO)

    if (ngx_eventfd != -1) {

        if (io_destroy(ngx_aio_ctx) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "io_destroy() failed");
        }

        if (close(ngx_eventfd) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "eventfd close() failed");
        }

        ngx_eventfd = -1;
    }

    ngx_aio_ctx = 0;

#endif

    // 释放事件数组占用的内存
    ngx_free(event_list);

    event_list = NULL;
    nevents = 0;
}


/**
 * 向epoll中添加事件
 *
 * *ev: 要添加的事件对象
 * event: 要添加的事件类型,读(EPOLLIN|EPOLLRDHUP)或写(EPOLLOUT)
 * flags: 事件触发方式,水平触发(NGX_LEVEL_EVENT),边缘触发(NGX_CLEAR_EVENT)
 *
 */
static ngx_int_t
ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    int                  op;
    uint32_t             events, prev;
    ngx_event_t         *e;
    ngx_connection_t    *c;
    struct epoll_event   ee;

    // 取出该事件对应的连接
    c = ev->data;

    // 取出要添加的事件类型
    events = (uint32_t) event;

    if (event == NGX_READ_EVENT) {
        /*
         * 如果当前要添加的是读事件
         * 因为还不确定写事件是否已经在epoll中,所以先把写事件和对应的写事件类型(EPOLLOUT)放入到相应的变量中
         */
        e = c->write;
        prev = EPOLLOUT;
#if (NGX_READ_EVENT != EPOLLIN|EPOLLRDHUP)
        // epoll中的读事件类型就是这两个,如果不是则强制改为这两个
        events = EPOLLIN|EPOLLRDHUP;
#endif

    } else {
        /**
         * 如果当前要添加的是写事件
         * 因为还不确定读事件是否已经在epoll中,所以先把读事件和对应的读事件类型(EPOLLIN|EPOLLRDHUP)放入到相应的变量中
         */
        e = c->read;
        prev = EPOLLIN|EPOLLRDHUP;
#if (NGX_WRITE_EVENT != EPOLLOUT)
        // epoll中的写事件类型就是这个,如果不是则强制改为这个
        events = EPOLLOUT;
#endif
    }

    /*
     * 如果当前要添加的是读事件,那么e就代表连接的写事件
     * 如果当前要添加的是写事件,那么e就代表连接的读事件
     * e和prev这两个变量是为了记录,连接c在epoll中的旧事件
     */
    if (e->active) {
        // 如果连接c在epoll中存在旧事件,则这次的操作就是修改
        op = EPOLL_CTL_MOD;
        events |= prev;

    } else {
        op = EPOLL_CTL_ADD;
    }

    // 注册事件和事件的触发模式(边缘和水平)
    ee.events = events | (uint32_t) flags;

    /* 将连接c放入到epoll_event对象中
     * 因为在ngx中所有的地址都是对齐的,也就是说所有的地址最后一位肯定是0,
     * 而0和任意数n按位与的结果仍然是n,所以c和instance按位与后,c的最后一位就是instance的值。
     * 所以这一步操作的另外一个意义是,把当前事件中的instance标志位也保存在了epoll_event对象中。
     *
     * 这时候epoll_event.data.ptr就保存了c的地址(使用时最后一位要变成0)和instance标志位两个值。
     *
     */
    ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "epoll add event: fd:%d op:%d ev:%08XD",
                   c->fd, op, ee.events);

    // 将事件添加(或更新)到ep(epoll对象)中,op是要做的操作(添加或修改),c->fd是文件描述符,ee就是事件对象
    if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "epoll_ctl(%d, %d) failed", op, c->fd);
        return NGX_ERROR;
    }

    // 将当前事件标记为已经存在epoll对象中的事件
    ev->active = 1;
#if 0
    ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;
#endif

    return NGX_OK;
}


/**
 * 从epoll中删除一个事件(读或写)
 *
 * *ev: 要操作的事件对象,该对象代表ngx中的一个读或写对象
 * event: 事件类型,是读事件还是写事件
 * flags: 事件触发方式(水平或边缘) | NGX_CLOSE_EVENT | ?
 *
 */
static ngx_int_t
ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    int                  op;
    uint32_t             prev;
    ngx_event_t         *e;
    ngx_connection_t    *c;
    struct epoll_event   ee;

    /*
     * when the file descriptor is closed, the epoll automatically deletes
     * it from its queue, so we do not need to delete explicitly the event
     * before the closing the file descriptor
     */

    // NGX_CLOSE_EVENT标记代表后续会对该事件中的fd做关闭操作? TODO
    if (flags & NGX_CLOSE_EVENT) {
        ev->active = 0;
        return NGX_OK;
    }

    // 取出事件对象ev中的连接
    c = ev->data;

    if (event == NGX_READ_EVENT) {
        /*
         * 如果当前要删除的是读事件
         * 因为还不确定写事件是否已经在epoll中,所以先把写事件对象和对应的写事件类型(EPOLLOUT)放入到相应的变量中
         * 后续如果确定写事件在epoll中,那么就通过EPOLL_CTL_MOD来删除epoll中的读事件
         */
        e = c->write;
        prev = EPOLLOUT;

    } else {
        /*
         * 如果当前要删除的是写事件
         * 因为还不确定读事件是否已经在epoll中,所以先把读事件对象和对应的读事件类型(EPOLLIN|EPOLLRDHUP)放入到相应的变量中
         * 后续如果确定读事件在epoll中,那么就通过EPOLL_CTL_MOD来删除epoll中的写事件
         */
        e = c->read;
        prev = EPOLLIN|EPOLLRDHUP;
    }

    /*
     * 如果当前要删除的是读事件,那么e就代表c(连接)的写事件
     * 如果当前要删除的是写事件,那么e就代表c(连接)的读事件
     *
     * e和prev这两个变量是为了记录,连接c在epoll中的旧事件
     */
    if (e->active) {
        // 修改操作,保留原来的旧事件
        op = EPOLL_CTL_MOD;
        // 保留原来在epoll中的事件,并增加一个flags标记(有可能是水平触发或者边沿触发)
        ee.events = prev | (uint32_t) flags;
        ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);

    } else {
        // 删除操作
        op = EPOLL_CTL_DEL;
        ee.events = 0;
        ee.data.ptr = NULL;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "epoll del event: fd:%d op:%d ev:%08XD",
                   c->fd, op, ee.events);

    // 将事件(读或写)从ep(epoll对象)中删除掉,c->fd是文件描述符,ee就是事件对象
    if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "epoll_ctl(%d, %d) failed", op, c->fd);
        return NGX_ERROR;
    }

    // 因为ev对应的事件已经不再epoll中了,所以active标记为0
    ev->active = 0;

    return NGX_OK;
}


/**
 * 将一个连接添加到epoll对象中
 * 因为一个连接即关联了读事件又关联了写事件,所以该方法会把对应的读写事件都放入到epoll中
 *
 */
static ngx_int_t
ngx_epoll_add_connection(ngx_connection_t *c)
{
    struct epoll_event  ee;

    // 注册读写事件和边缘触(EPOLLET)发机制
    ee.events = EPOLLIN|EPOLLOUT|EPOLLET|EPOLLRDHUP;
    /*
     * 将ngx中的连接(c)地址放入到epoll_event对象中,地址的最后一位保存读事件的instance标志位
     * TODO 为啥是读事件的instance标志位,为啥不是写事件的标志位,难道是因为新连接都是先操作读事件?
     */
    ee.data.ptr = (void *) ((uintptr_t) c | c->read->instance);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "epoll add connection: fd:%d ev:%08XD", c->fd, ee.events);

    // 执行添加操作
    if (epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ee) == -1) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      "epoll_ctl(EPOLL_CTL_ADD, %d) failed", c->fd);
        return NGX_ERROR;
    }

    // 因为读写事件都放在了epoll中,所以事件的active都标记为1
    c->read->active = 1;
    c->write->active = 1;

    return NGX_OK;
}


/**
 *  从epoll中删除一个连接
 *
 *  *c: 要删除的连接
 *  flags: NGX_CLOSE_EVENT | ?
 *
 */
static ngx_int_t
ngx_epoll_del_connection(ngx_connection_t *c, ngx_uint_t flags)
{
    int                 op;
    struct epoll_event  ee;

    /*
     * when the file descriptor is closed the epoll automatically deletes
     * it from its queue so we do not need to delete explicitly the event
     * before the closing the file descriptor
     */

    if (flags & NGX_CLOSE_EVENT) {
        c->read->active = 0;
        c->write->active = 0;
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "epoll del connection: fd:%d", c->fd);

    // 要做删除操作
    op = EPOLL_CTL_DEL;
    ee.events = 0;
    ee.data.ptr = NULL;

    // 从epoll中删除c->fd对应的事件
    if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      "epoll_ctl(%d, %d) failed", op, c->fd);
        return NGX_ERROR;
    }

    // c对应的读写事件已经从epoll中删除,所以对应的active设置为0
    c->read->active = 0;
    c->write->active = 0;

    return NGX_OK;
}


#if (NGX_HAVE_EVENTFD)

static ngx_int_t
ngx_epoll_notify(ngx_event_handler_pt handler)
{
    static uint64_t inc = 1;

    notify_event.data = handler;

    if ((size_t) write(notify_fd, &inc, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ngx_log_error(NGX_LOG_ALERT, notify_event.log, ngx_errno,
                      "write() to eventfd %d failed", notify_fd);
        return NGX_ERROR;
    }

    return NGX_OK;
}

#endif


/**
 * 调用epoll_wait方法处理事件
 *
 * *cycle: 有关进程的信息
 * timer: 调用epoll_wait时要等待的时间
 * flags: NGX_UPDATE_TIME | NGX_POST_EVENTS
 *
 */
static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    int                events;
    uint32_t           revents;
    ngx_int_t          instance, i;
    ngx_uint_t         level;
    ngx_err_t          err;
    ngx_event_t       *rev, *wev;
    ngx_queue_t       *queue;
    ngx_connection_t  *c;

    /* NGX_TIMER_INFINITE == INFTIM */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "epoll timer: %M", timer);

    // 获取就绪的事件
    events = epoll_wait(ep, event_list, (int) nevents, timer);

    // 如果epoll_wait为正常返回,则设置err为ngx_errno,否则设置为0(零表示正常返回)
    err = (events == -1) ? ngx_errno : 0;

    // 更新缓存时间
    if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
        ngx_time_update();
    }

    // 如果发生了错误
    if (err) {
        // 如果epoll_wait是被一个信号中断的则走这里
        if (err == NGX_EINTR) {
            /*
             * 如果epoll_wait是被SIGALRM信号中断的,则走下面这段逻辑,因为目前ngx中只有该信号绑定的
             * ngx_timer_signal_handler方法才会设置变量ngx_event_timer_alarm为1
             */
            if (ngx_event_timer_alarm) {
                // 此次调用被中断的目的就是去更新缓存时间,而缓存时间的更新已经在上面做过了,所以直接返回NGX_OK。

                ngx_event_timer_alarm = 0;
                return NGX_OK;
            }

            level = NGX_LOG_INFO;

            //如果是有其它信号中断的,则最终会返回错误,比如其它连接错误等。
        } else {
            level = NGX_LOG_ALERT;
        }

        ngx_log_error(level, cycle->log, err, "epoll_wait() failed");
        return NGX_ERROR;
    }

    if (events == 0) {
        /*
         * 如果epoll_wait设置的超时时间不是-1(NGX_TIMER_INFINITE),则说明是epoll_wait超时了
         * 超时后什么也不做,直接返回NGX_OK
         */
        if (timer != NGX_TIMER_INFINITE) {
            return NGX_OK;
        }

        /**
         * 如果epoll_wait设置的超时时间是-1(NGX_TIMER_INFINITE),且没有返回一个可用的就绪事件,则表明发生了其它错误
         */
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "epoll_wait() returned no events without timeout");
        return NGX_ERROR;
    }

    /* 开始处理返回的就绪事件 */

    for (i = 0; i < events; i++) {
        // 取出这个事件对应的链接
        c = event_list[i].data.ptr;

        // 取出instance标记位,这个标记代表该事件对应的连接c是否被释放过,用它可以标记事件是否新鲜
        instance = (uintptr_t) c & 1;
        /*
         * 还原c连接地址(将c的最后一位置为0)
         * ~1表示把1按位取反,取反后就是 00001,该值于任何数据按与操作则最后一位都会被置为零
         */
        c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);

        // 取出该链接的读事件
        rev = c->read;

        if (c->fd == -1 || rev->instance != instance) {

            /*
             * the stale event from a file descriptor
             * that was just closed in this iteration
             *
             * 使用instance貌似无法判断fd是否关闭过(连接c是否被释放过):
             * 假设现在有5个事件对象,分别表示为e1~e5; 操作系统的事件(event_poll),非ngx中的事件
             * 有3个连接对象,分别为c1、c3、c5
             * 有一个用于监听的文件描述符,用fd_l1表示
             *
             * 现在e1、e3、e5分别关联着c1、c3、c5
             * e2、e4是有监听描述符fd_l1产生的
             *
             * e1做完事之后,关闭了e5对应的c5; 此时e5中的instance = 0; c5中的instance = 0; (这一步是前事件关闭后事件的连接)
             *
             * e2是一个新建连接操作,获取新fd后,从连接池中有获取了c5, 此时c5中的instance = 1;
             *
             * e3是一个普通事件操作,他做完事之后,又把e2刚创建的连接c5给关闭了(这一步是关键),此时c5中的instance = 1; (这一步是后事件关闭前事件的连接)
             *
             * e4又是一个新建连接操作,获取新fd后,从连接池中又获取了c5,此时c5中的instance = 0;
             *
             * 此时e5中关联的c5又复活了,并且e5中的instance和c5中的instance是一致的;
             * 实际上e5对应的文件描述符早在e1做完事之后就关闭了,e5这个事件应该早就过期了,但是这里且无法区分是否过期。
             *
             */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "epoll: stale event %p", c);

            // 如果事件不新鲜了就不用管他了,也不用主动从epoll中删除该事件,因为一个关闭的fd会主动从epoll中删除自己注册的事件
            continue;
        }

        // 当前事件就绪的事件类型(读事件或写事件)
        revents = event_list[i].events;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "epoll: fd:%d ev:%04XD d:%p",
                       c->fd, revents, event_list[i].data.ptr);

        if (revents & (EPOLLERR|EPOLLHUP)) {
            /*
             * EPOLLERR: 连接发生错误
             * EPOLLHUP: 连接被挂起
             *
             * 这两个事件不需要主动向epoll对象注册,默认就会存在这两个事件
             *
             */
            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "epoll_wait() error on fd:%d ev:%04XD",
                           c->fd, revents);
        }

#if 0
        if (revents & ~(EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "strange epoll_wait() events fd:%d ev:%04XD",
                          c->fd, revents);
        }
#endif

        if ((revents & (EPOLLERR|EPOLLHUP))
             && (revents & (EPOLLIN|EPOLLOUT)) == 0)
        {
            /*
             * if the error events were returned without EPOLLIN or EPOLLOUT,
             * then add these flags to handle the events at least in one
             * active handler
             */

            /*
             * 事件对应的连接发生了错误,或者连接被挂起,并且返回的事件类型中也不存在EPOLLIN或EPOLLOUT
             * 如果这里不做任何事情的话,后续方法就永远无法感知这个连接的错误,这个连接可能就永远无法的到释放。
             *
             * 这里解决的办法是假定EPOLLIN和EPOLLOUT,这两个事件都发生了,再通过判断哪一个事件在epoll中来
             * 决定执行哪一个具体事件绑定的方法。
             *
             * 这个方法对用户屏蔽了EPOLLERR|EPOLLHUP这两个事件的存在,用户在具体执行自己的逻辑的时候才会感知到
             * 连接发生了错误,然后用户就可以做逻辑在处理这个问题。
             *
             */

            revents |= EPOLLIN|EPOLLOUT;
        }

        // 是读事件,并且该事件在epoll中; 这里对事件active标记的判断就是用来屏蔽EPOLLERR和EPOLLHUP这两个错误事件的方法
        if ((revents & EPOLLIN) && rev->active) {

#if (NGX_HAVE_EPOLLRDHUP)
            if (revents & EPOLLRDHUP) {
                // TODO 紧急数据
                rev->pending_eof = 1;
            }
#endif

            // 设置有数据可读
            rev->ready = 1;

            /*
             * post事件,和/src/event/ngx_event.c/ngx_accept_mutex_held变量配合使用,
             * 该值为1,则falgs就会被打上NGX_POST_EVENTS标。
             */

            if (flags & NGX_POST_EVENTS) {
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;

                // 把事件添加到相应的延迟队列中
                ngx_post_event(rev, queue);

            } else {
                // 调用读事件的回调方法(读事件包括新建立连接事件;ngx_event_accept)
                rev->handler(rev);
            }
        }

        // 取出写事件
        wev = c->write;

        if ((revents & EPOLLOUT) && wev->active) {

            if (c->fd == -1 || wev->instance != instance) {
                /*
                 * 和读做同样的判断，判断fd是否被关闭,以及事件是否新鲜
                 */

                /*
                 * the stale event from a file descriptor
                 * that was just closed in this iteration
                 */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                               "epoll: stale event %p", c);
                continue;
            }

            // 设置可写数据标志
            wev->ready = 1;

            // post事件?
            if (flags & NGX_POST_EVENTS) {
                ngx_post_event(wev, &ngx_posted_events);

            } else {
                // 调用写事件的回调方法
                wev->handler(wev);
            }
        }
    }

    return NGX_OK;
}


#if (NGX_HAVE_FILE_AIO)

static void
ngx_epoll_eventfd_handler(ngx_event_t *ev)
{
    int               n, events;
    long              i;
    uint64_t          ready;
    ngx_err_t         err;
    ngx_event_t      *e;
    ngx_event_aio_t  *aio;
    struct io_event   event[64];
    struct timespec   ts;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "eventfd handler");

    n = read(ngx_eventfd, &ready, 8);

    err = ngx_errno;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "eventfd: %d", n);

    if (n != 8) {
        if (n == -1) {
            if (err == NGX_EAGAIN) {
                return;
            }

            ngx_log_error(NGX_LOG_ALERT, ev->log, err, "read(eventfd) failed");
            return;
        }

        ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
                      "read(eventfd) returned only %d bytes", n);
        return;
    }

    ts.tv_sec = 0;
    ts.tv_nsec = 0;

    while (ready) {

        events = io_getevents(ngx_aio_ctx, 1, 64, event, &ts);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "io_getevents: %l", events);

        if (events > 0) {
            ready -= events;

            for (i = 0; i < events; i++) {

                ngx_log_debug4(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                               "io_event: %uXL %uXL %L %L",
                                event[i].data, event[i].obj,
                                event[i].res, event[i].res2);

                e = (ngx_event_t *) (uintptr_t) event[i].data;

                e->complete = 1;
                e->active = 0;
                e->ready = 1;

                aio = e->data;
                aio->res = event[i].res;

                ngx_post_event(e, &ngx_posted_events);
            }

            continue;
        }

        if (events == 0) {
            return;
        }

        /* events == -1 */
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "io_getevents() failed");
        return;
    }
}

#endif


/**
 * 创建用于保存epoll配置信息的结构体
 */
static void *
ngx_epoll_create_conf(ngx_cycle_t *cycle)
{
    ngx_epoll_conf_t  *epcf;

    epcf = ngx_palloc(cycle->pool, sizeof(ngx_epoll_conf_t));
    if (epcf == NULL) {
        return NULL;
    }

    epcf->events = NGX_CONF_UNSET;
    epcf->aio_requests = NGX_CONF_UNSET;

    return epcf;
}


/**
 *
 * 初始化配置信息events和aio_requests
 *
 */
static char *
ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_epoll_conf_t *epcf = conf;

    ngx_conf_init_uint_value(epcf->events, 512);
    ngx_conf_init_uint_value(epcf->aio_requests, 32);

    return NGX_CONF_OK;
}
