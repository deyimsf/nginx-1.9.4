
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define DEFAULT_CONNECTIONS  512


extern ngx_module_t ngx_kqueue_module;
extern ngx_module_t ngx_eventport_module;
extern ngx_module_t ngx_devpoll_module;
extern ngx_module_t ngx_epoll_module;
extern ngx_module_t ngx_select_module;


static char *ngx_event_init_conf(ngx_cycle_t *cycle, void *conf);
static ngx_int_t ngx_event_module_init(ngx_cycle_t *cycle);
static ngx_int_t ngx_event_process_init(ngx_cycle_t *cycle);
static char *ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static void *ngx_event_core_create_conf(ngx_cycle_t *cycle);
static char *ngx_event_core_init_conf(ngx_cycle_t *cycle, void *conf);


// 更新缓存时间的间隔时间
static ngx_uint_t     ngx_timer_resolution;
// 可以更新缓存时间,在ngx_timer_signal_handler方法中设置
sig_atomic_t          ngx_event_timer_alarm;

static ngx_uint_t     ngx_event_max_module;

ngx_uint_t            ngx_event_flags;

/*
 * 有具体事件实现模块负责设置该值,如 ngx_event_actions = ngx_epoll_module_ctx.actions;
 * 该变量中的方法用于操作事件驱动器中的事件
 */
ngx_event_actions_t   ngx_event_actions;


static ngx_atomic_t   connection_counter = 1;
// 总共建立的连接个数(671行该指针指向了共享内存)
ngx_atomic_t         *ngx_connection_counter = &connection_counter;

// 映射的共享内存地址
ngx_atomic_t         *ngx_accept_mutex_ptr;
// 共享缓存锁
ngx_shmtx_t           ngx_accept_mutex;
/*
 * 是否使用互斥锁(accept_mutex),1使用, 0不使用
 * 如果开启了accept_mutex锁,并且当前进程不是master进程,并且worker的个数大于1,才会使用这个锁
 */
ngx_uint_t            ngx_use_accept_mutex;
// ngx_eventport_module事件模块用到的
ngx_uint_t            ngx_accept_events;
// 表示当前进程是否获取到互斥锁,1获取到了,0没有获取到; 刚开始的时候所有的worker都没有获取到该锁
ngx_uint_t            ngx_accept_mutex_held;
ngx_msec_t            ngx_accept_mutex_delay;
ngx_int_t             ngx_accept_disabled;


#if (NGX_STAT_STUB)

/*
 * 如果没有开启master-worker模式,指针变量ngx_stat_accepted会指向ngx_stat_accepted0对应的地址
 *
 * 如果开启了master-worker模式,指针变量ngx_stat_accepted会指向共享内存地址:
 * 		ngx_stat_accepted = (ngx_atomic_t *) (shared + 3 * cl);
 */
ngx_atomic_t   ngx_stat_accepted0;
ngx_atomic_t  *ngx_stat_accepted = &ngx_stat_accepted0;

ngx_atomic_t   ngx_stat_handled0;
ngx_atomic_t  *ngx_stat_handled = &ngx_stat_handled0;
ngx_atomic_t   ngx_stat_requests0;
ngx_atomic_t  *ngx_stat_requests = &ngx_stat_requests0;
ngx_atomic_t   ngx_stat_active0;
ngx_atomic_t  *ngx_stat_active = &ngx_stat_active0;
ngx_atomic_t   ngx_stat_reading0;
ngx_atomic_t  *ngx_stat_reading = &ngx_stat_reading0;
ngx_atomic_t   ngx_stat_writing0;
ngx_atomic_t  *ngx_stat_writing = &ngx_stat_writing0;
ngx_atomic_t   ngx_stat_waiting0;
ngx_atomic_t  *ngx_stat_waiting = &ngx_stat_waiting0;

#endif


/**
 * 核心模块ngx_events_module拥有的命令
 *
 */
static ngx_command_t  ngx_events_commands[] = {

    { ngx_string("events"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_events_block,
      0,
      0,
      NULL },

      ngx_null_command
};


/**
 * 声明一个核心模块上下文,这个核心模块的上下文叫events
 *
 */
static ngx_core_module_t  ngx_events_module_ctx = {
	// 模块名字
    ngx_string("events"),
    NULL,
    ngx_event_init_conf
};


/**
 * 声明一个模块,这个模块是一个核心模块
 *
 * 这个核心模块是用来处理事件的
 *
 */
ngx_module_t  ngx_events_module = {
    NGX_MODULE_V1,
    &ngx_events_module_ctx,                /* module context */
    ngx_events_commands,                   /* module directives */
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


static ngx_str_t  event_core_name = ngx_string("event_core");


/**
 * 事件模块ngx_event_core_module拥有的命令
 */
static ngx_command_t  ngx_event_core_commands[] = {

    { ngx_string("worker_connections"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_connections,
      0,
      0,
      NULL },

    { ngx_string("use"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_use,
      0,
      0,
      NULL },

    { ngx_string("multi_accept"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, multi_accept),
      NULL },

    { ngx_string("accept_mutex"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex),
      NULL },

    { ngx_string("accept_mutex_delay"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex_delay),
      NULL },

    { ngx_string("debug_connection"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_debug_connection,
      0,
      0,
      NULL },

      ngx_null_command
};


/**
 * 声明一个事件模块上下文,这个事件模块的上下文叫event_core
 *
 */
ngx_event_module_t  ngx_event_core_module_ctx = {
    &event_core_name,
    ngx_event_core_create_conf,            /* create configuration */
    ngx_event_core_init_conf,              /* init configuration */

    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};


/**
 * 声明一个事件模块
 *
 * 这个事件模块用来管理其它事件模块,比如使用哪个具体事件模型(epoll、select等)
 *
 */
ngx_module_t  ngx_event_core_module = {
    NGX_MODULE_V1,
    &ngx_event_core_module_ctx,            /* module context */
    ngx_event_core_commands,               /* module directives */
    NGX_EVENT_MODULE,                      /* module type */
    NULL,                                  /* init master */
    ngx_event_module_init,                 /* init module */
    ngx_event_process_init,                /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/**
 * 间接调用epoll_wait方法处理监听连接事件和普通网络事件
 * 处理时间事件
 */
void
ngx_process_events_and_timers(ngx_cycle_t *cycle)
{
    ngx_uint_t  flags;
    ngx_msec_t  timer, delta;

    /*
     * 如果使用定时器设置了定期更新缓存时间的机制(使用SIGALRM信号绑定的ngx_timer_signal_handler方法)
     * 那么就把epoll_wait的超时时间(timer)设置为无穷大(NGX_TIMER_INFINITE)
     *
     * 具体事件模块(比如epoll)使用ngx_event_timer_alarm标志来决定是否调用ngx_time_update方法
     */
    if (ngx_timer_resolution) {
        timer = NGX_TIMER_INFINITE;
        flags = 0;

    } else {
    	/*
    	 * 用户没有自定义更新ngx缓存时间的机制,那么就从事件定时器中查找一个最小的时间来决定
    	 *
    	 * 具体事件模块(比如epoll)使用NGX_UPDATE_TIME标志来决定收调用ngx_time_update方法
    	 */
        timer = ngx_event_find_timer();
        flags = NGX_UPDATE_TIME;

#if (NGX_WIN32)

        /* handle signals from master in case of network inactivity */

        if (timer == NGX_TIMER_INFINITE || timer > 500) {
            timer = 500;
        }

#endif
    }

    // 处理负载和惊群问题
    if (ngx_use_accept_mutex) {
    	// 处理负载问题
        if (ngx_accept_disabled > 0) {
        	/*
        	 * 所有worker刚启动后不会走这个逻辑,因为刚开始ngx_accept_disabled肯定是负数。
        	 *
        	 * 如果当前负载值大于零,表示当前woker已经很忙了,所以就没必要再去获取锁了,获取不了锁
        	 * 也就无法将监听连接对应的读事件放入epoll中,所以后续就只能处理普通连接的事件。
        	 *
        	 * 所以一旦处理的连接过多后,worker就不再去抢锁,不抢锁就不会有新连接去处理。
        	 */
            ngx_accept_disabled--;

        // 处理惊群问题
        } else {
        	/*
        	 * 所有woker刚启动的时候肯定没有负载问题,也就是说ngx_accept_disabled值肯定小于零
        	 * 这时候第一个接收连接的worker肯定会获取到这个锁(ngx_accept_mutex_held=1)。
        	 *
        	 * 拿到互斥锁的worker会利用ngx_posted_accept_events和ngx_posted_events这两个队列,快速的释放锁。
        	 * 其实就是把从epoll_wait中获取到的事件都放入到这两个队列中,而不是立即执行其对应的回调方法。
        	 *
        	 * 这里有另一需要注意的地方:
        	 *  只要使用互斥锁,那么刚开始的时候,所有worker的epoll中都不会存在监听连接事件,
        	 *  所以如果没有不去获取一次锁,则永远都不会有网络事件处理.
        	 *
        	 *  只有当一个worker获取锁后,才会开始去处理监听连接事件,只有处理过一次监听连接事件并建立起新连接后,
        	 *  才会有普通连接事件。
        	 *
        	 */
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) {
                return;
            }

            // 获取到锁之后就设置该worker支持post事件机制(NGX_POST_EVENTS)
            if (ngx_accept_mutex_held) {
                flags |= NGX_POST_EVENTS;

            } else {
            	// 获取锁失败后,后续在调用epoll_wait方法时,会设置超时时间是timer
                if (timer == NGX_TIMER_INFINITE
                    || timer > ngx_accept_mutex_delay)
                {
                    timer = ngx_accept_mutex_delay;
                }
            }
        }
    }

    delta = ngx_current_msec;

    /*
     * 处理网络事件
     * 调用epoll_wait方法处理事件
     *
     * 如果对应的读事件是监听连接的,就说明需要建立新连接了,建立新连接使用ngx_event_accept方法
     * 该方法会设置ngx_accept_disabled值,这是一个开启负载均衡的阀值,每调用一次ngx_event_accept方法
     * 都会更新一次ngx_accept_disabled的值。
     *
     * 只有获取锁的woker才有NGX_POST_EVENTS标记,所以获取到锁的worker的ngx_accept_mutex_held值为1(表示该woker获取到锁了)
     * 获取到锁的woker不会立即执行相应的事件回调方法,会将其放入到ngx_posted_accept_events或ngx_posted_events这两个队列中。
     * 这样做是为了快速的释放锁,避免其它worker因为获取不到锁而无法执行正常的事件操作。
     *
     * 所以如果不开互斥锁的话,也就不会有处理负载问题,这样每个woker拿到事件后就会立即执行相应的回调方法。
     */
    (void) ngx_process_events(cycle, timer, flags);

    delta = ngx_current_msec - delta;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "timer delta: %M", delta);

    // 1.处理存放在ngx_posted_accept_events队列中的读事件来接收新连接
    ngx_event_process_posted(cycle, &ngx_posted_accept_events);

    /*
     * 处理完新连接之后立即释放锁,这样其它worker就有机会获得锁了。
     *
     * 但是这里并没有及时的设置ngx_accept_mutex_held=0,这是因为,虽然锁已经释放了,但是
     * 所有监听连接的读事件都还在epoll中,所以ngx_accept_mutex_held变量的另一个意思表示
     * 该worker进程中epoll事件启动器中,还存在监听连接事件。
     *
     * 实际上监听连接事件的删除和该变量的重置,是在ngx_trylock_accept_mutex方法中进行的。
     * 当该worker进程下次去获取锁的时候,如果没有获取成功,并且ngx_accept_mutex_held的值
     * 还是1,这时就会把所有监听连接的事件从epoll中删除,并且重置ngx_accept_mutex_held值
     * 为0。
     */
    if (ngx_accept_mutex_held) {
        ngx_shmtx_unlock(&ngx_accept_mutex);
    }

    // 2.处理时间事件
    if (delta) {
    	// 处理时间事件
        ngx_event_expire_timers();
    }

    // 3.处理延迟的普通读写事件
    ngx_event_process_posted(cycle, &ngx_posted_events);
}


/**
 * 向事件框架添加读事件
 */
ngx_int_t
ngx_handle_read_event(ngx_event_t *rev, ngx_uint_t flags)
{
    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        /* kqueue, epoll */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_CLEAR_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }
        }

        return NGX_OK;

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {

        /* select, poll, /dev/poll */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (rev->active && (rev->ready || (flags & NGX_CLOSE_EVENT))) {
            if (ngx_del_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT | flags)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

    } else if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT) {

        /* event ports */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (rev->oneshot && !rev->ready) {
            if (ngx_del_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }

    /* iocp */

    return NGX_OK;
}


/**
 * 向事件驱动器中添加写事件
 *
 * *wev: 要添加的写事件
 * lowat: This directive is ignored on Linux, Solaris, and Windows.
 * 		./src/http/ngx_http_core_module.c中有send_lowat指令
 */
ngx_int_t
ngx_handle_write_event(ngx_event_t *wev, size_t lowat)
{
    ngx_connection_t  *c;

    if (lowat) {
        c = wev->data;

        if (ngx_send_lowat(c, lowat) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        /* kqueue, epoll */

        if (!wev->active && !wev->ready) {
            if (ngx_add_event(wev, NGX_WRITE_EVENT,
                              NGX_CLEAR_EVENT | (lowat ? NGX_LOWAT_EVENT : 0))
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }
        }

        return NGX_OK;

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {

        /* select, poll, /dev/poll */

        if (!wev->active && !wev->ready) {
            if (ngx_add_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (wev->active && wev->ready) {
            if (ngx_del_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

    } else if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT) {

        /* event ports */

        if (!wev->active && !wev->ready) {
            if (ngx_add_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (wev->oneshot && wev->ready) {
            if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }

    /* iocp */

    return NGX_OK;
}


static char *
ngx_event_init_conf(ngx_cycle_t *cycle, void *conf)
{
	// 检查事件模块的配置信息是否存在
    if (ngx_get_conf(cycle->conf_ctx, ngx_events_module) == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "no \"events\" section in configuration");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/**
 * 设置事件模块的初始化信息
 *
 * 还没有fork出worker进程时在方法/src/core/ngx_cycle.c/ngx_init_cycle中
 * 调用的ngx_module_t->init_module方法。
 *
 * ngx_event_process_init方法则是在fork出worker进程之后调用
 */
static ngx_int_t
ngx_event_module_init(ngx_cycle_t *cycle)
{
    void              ***cf;
    u_char              *shared;
    size_t               size, cl;
    ngx_shm_t            shm;
    ngx_time_t          *tp;
    ngx_core_conf_t     *ccf;
    ngx_event_conf_t    *ecf;

    /*
     * 获取核心模块(ngx_events_module)的配置信息指针,核心模块指针都存放在conf_ctx的第二层指针上
     * 这里用cf这指针貌似没啥用,获取ecf的话直接用ngx_event_get_conf(cycle->conf_ctx,ngx_event_core_module)就可以
     */
    cf = ngx_get_conf(cycle->conf_ctx, ngx_events_module);
    // 获取ngx_event_core_module模块的配置信息(如worker_connections、use等)
    ecf = (*cf)[ngx_event_core_module.ctx_index];

    if (!ngx_test_config && ngx_process <= NGX_PROCESS_MASTER) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "using the \"%s\" event method", ecf->name);
    }

    // 获取核心模块(ngx_core_module)的配置信息指针
    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    // 设置更新缓存时间的间隔时间
    ngx_timer_resolution = ccf->timer_resolution;

    //下面这段代码貌似只是为了打印日志,比如分配的ngx_connection_t个数大于当前进程允许打开的最大描述符个数
#if !(NGX_WIN32)
    {
    ngx_int_t      limit;
    struct rlimit  rlmt;

    if (getrlimit(RLIMIT_NOFILE, &rlmt) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "getrlimit(RLIMIT_NOFILE) failed, ignored");

    } else {
    	/**
    	 * 如果用户指定的worker_connections数，大于当前进程允许打开的最大描述符,并且指令worker_rlimit_nofile也没有设置值;
    	 * 或者说worker_rlimit_nofile指令设置值了,但是指令worker_connections的值要大于worker_rlimit_nofile的值
    	 *
    	 * 简单说明就是limit的值不能大于worker_connections和worker_rlimit_nofile两个指令中最大的那个的值
    	 */
        if (ecf->connections > (ngx_uint_t) rlmt.rlim_cur
            && (ccf->rlimit_nofile == NGX_CONF_UNSET
                || ecf->connections > (ngx_uint_t) ccf->rlimit_nofile))
        {
            limit = (ccf->rlimit_nofile == NGX_CONF_UNSET) ?
                         (ngx_int_t) rlmt.rlim_cur : ccf->rlimit_nofile;

            ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                          "%ui worker_connections exceed "
                          "open file resource limit: %i",
                          ecf->connections, limit);
        }
    }
    }
#endif /* !(NGX_WIN32) */


    // 如果没有开启master-worker模式,则不需要执行后续代码
    if (ccf->master == 0) {
    	/*
    	 * 没有开启master-worker模块式,像ngx_stat_accepted、ngx_stat_handled等这样的指针变量,
    	 * 则会直接指向ngx_stat_accepted0、ngx_stat_handled0这样的全局变量地址。
    	 */
        return NGX_OK;
    }

    /*
     * 如果该变量不等于0,就代表已经分配完共享内存了,不需要在分配了,直接返回就ok
     * 比如reload的时候,共享内存是不会消失的,因为共享内存是在master进程中分配的
     */
    if (ngx_accept_mutex_ptr) {
        return NGX_OK;
    }


    /* cl should be equal to or greater than cache line size */
    /* 每个变量占用的内存,大于等于cpu缓存行 */

    // 第一个128字节存放共享缓存(ngx_accept_mutex)中的lock(ngx_shmtx_sh_t->lock)
    cl = 128;

    size = cl            /* ngx_accept_mutex */
           + cl          /* ngx_connection_counter */ //记录连接个数
           + cl;         /* ngx_temp_number */

#if (NGX_STAT_STUB)

    size += cl           /* ngx_stat_accepted */
           + cl          /* ngx_stat_handled */
           + cl          /* ngx_stat_requests */
           + cl          /* ngx_stat_active */
           + cl          /* ngx_stat_reading */
           + cl          /* ngx_stat_writing */
           + cl;         /* ngx_stat_waiting */

#endif

    /**共享内存,所有进程都可以看到的内存地址*/
    // 设置共享内存大小
    shm.size = size;
    // 共享内存名字的长度
    shm.name.len = sizeof("nginx_shared_zone") - 1;
    // 共享内存名字; 在这里的赋值,会把字符串放入到常量中,常量中的内存是无法改变的
    shm.name.data = (u_char *) "nginx_shared_zone";
    shm.log = cycle->log;

    // 为共享内存分配内存空间,使用mmap函数;这样所有的进程就都可以看到这块内存了
    if (ngx_shm_alloc(&shm) != NGX_OK) {
        return NGX_ERROR;
    }

    // 映射好的共享内存地址
    shared = shm.addr;
    ngx_accept_mutex_ptr = (ngx_atomic_t *) shared;

    /*
     * spin表示没有获取到锁时,可以循环获重试获取锁的次数,在这个次数内扔未获取到锁,
     * 则调用sched_yield方法休眠该进程。
     *
     * 方法/src/core/ngx_shmtx.c/ngx_shmtx_lock中会用到该值。
     */
    ngx_accept_mutex.spin = (ngx_uint_t) -1;

    /*
     * 共享缓存(ngx_accept_mutex)中的锁指针,指向分配好的共享缓存地址
     * (&ngx_accept_mutex)->lock = &(shared->lock)
     */
    if (ngx_shmtx_create(&ngx_accept_mutex, (ngx_shmtx_sh_t *) shared,
                         cycle->lock_file.data)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    /**下面的代码负责把共享内存的地址赋值给各个变量**/

    // 用来记录总的连接使用数的内存
    ngx_connection_counter = (ngx_atomic_t *) (shared + 1 * cl);

    (void) ngx_atomic_cmp_set(ngx_connection_counter, 0, 1);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "counter: %p, %d",
                   ngx_connection_counter, *ngx_connection_counter);

    ngx_temp_number = (ngx_atomic_t *) (shared + 2 * cl);

    tp = ngx_timeofday();

    ngx_random_number = (tp->msec << 16) + ngx_pid;

#if (NGX_STAT_STUB)

    ngx_stat_accepted = (ngx_atomic_t *) (shared + 3 * cl);
    ngx_stat_handled = (ngx_atomic_t *) (shared + 4 * cl);
    ngx_stat_requests = (ngx_atomic_t *) (shared + 5 * cl);
    ngx_stat_active = (ngx_atomic_t *) (shared + 6 * cl);
    ngx_stat_reading = (ngx_atomic_t *) (shared + 7 * cl);
    ngx_stat_writing = (ngx_atomic_t *) (shared + 8 * cl);
    ngx_stat_waiting = (ngx_atomic_t *) (shared + 9 * cl);

#endif

    return NGX_OK;
}


#if !(NGX_WIN32)

/**
 * 设置是否可以调用更新缓存时间的函数ngx_time_update
 *
 * signo: 信号
 */
static void
ngx_timer_signal_handler(int signo)
{
	// 可以更新缓存时间
    ngx_event_timer_alarm = 1;

#if 1
    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ngx_cycle->log, 0, "timer signal");
#endif
}

#endif


/**
 * fork出woker进程之后调用(ngx_module_t->init_process)
 * 	/src/os/unix/ngx_process_cycle.c/ngx_single_process_cycle
 * 	/src/os/unix/ngx_process_cycle.c/ngx_worker_process_init
 *
 * ngx_event_module_init方法则是fork出worker进程之前调用
 *
 * 1.设置是否使用互斥锁变量ngx_use_accept_mutex
 * 2.选定具体事件模块
 * 3.分配ngx_connection_t连接池内存空间,总共connection_n个
 * 4.分配读写事件对象内存空间
 *		cycle->read_events
 *		cycle->write_events
 * 5.连接对象和读写事件对象关联起来
 * 6.初始化cycle->listening中的监听连接,比如设置连接中读事件的回调方法(rev->handler = ngx_event_accept)
 *
 */
static ngx_int_t
ngx_event_process_init(ngx_cycle_t *cycle)
{
    ngx_uint_t           m, i;
    ngx_event_t         *rev, *wev;
    ngx_listening_t     *ls;
    ngx_connection_t    *c, *next, *old;
    ngx_core_conf_t     *ccf;
    ngx_event_conf_t    *ecf;
    ngx_event_module_t  *module;

    // 获取核心模块(ngx_core_module)的配置信息,模块ngx_core_module是一个核心模块
    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    // 获取核心事件模块(ngx_event_core_module)的配置信息,模块ngx_event_core_module是个事件模块
    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);

    /* 检查是否需要打开负载均衡锁
     * master: 是否是master-worker模式,如果不是就没必要使用互斥锁了
     * woker_processes: 如果只有一个worker进程就没必要开启负载了
     * accept_mutex: 明确指定是否开启互斥锁(是否使用负载均衡,默认不使用)
     */
    if (ccf->master && ccf->worker_processes > 1 && ecf->accept_mutex) {
    	// 使用互斥锁
        ngx_use_accept_mutex = 1;
        // 0表示没有获取到互斥锁
        ngx_accept_mutex_held = 0;
        /*
         * 对应accept_mutex_delay指令,获取(epoll_wait)就绪事件时最多可以等待的时间
         * 如果当前worker获取互斥锁失败,ngx_accept_mutex_delay值就有可能是接下来调用epoll_wait方法时传入的参数。
         * if (timer == NGX_TIMER_INFINITE || timer > ngx_accept_mutex_delay){
         *   timer = ngx_accept_mutex_delay;
         * }
         */
        ngx_accept_mutex_delay = ecf->accept_mutex_delay;

    } else {
    	// 不使用互斥锁
        ngx_use_accept_mutex = 0;
    }

#if (NGX_WIN32)

    /*
     * disable accept mutex on win32 as it may cause deadlock if
     * grabbed by a process which can't accept connections
     */

    ngx_use_accept_mutex = 0;

#endif

    /*
     * 这两个队列会配合互斥锁(accept_mutex)解决惊群问题
     *
     * ngx_posted_accept_events: 存放监听连接对应的读事件
     * ngx_posted_events: 存放普通连接对应的读写事件
     */
    ngx_queue_init(&ngx_posted_accept_events);
    ngx_queue_init(&ngx_posted_events);

    /*
     * 初始化定时器事件(ngx_event_timer/ngx_event_timer_rbtree),ngx_event_timer_rbtree是rbtree
     * 每个worker的定时器事件都放在ngx_event_timer_rbtree红黑树中了
     */
    if (ngx_event_timer_init(cycle->log) == NGX_ERROR) {
        return NGX_ERROR;
    }

    // 设置具体事件模块(比如epoll、kqueue等)
    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_EVENT_MODULE) {
            continue;
        }

        // 选定具体事件模块,如epoll
        if (ngx_modules[m]->ctx_index != ecf->use) {
            continue;
        }

        module = ngx_modules[m]->ctx;

        /*
         * 调用具体事件模块的init方法(对于ngx_epoll_module模块来说就是ngx_epoll_init方法)
         * 在ngx_epoll_init方法中会:
         *  1.创建epoll对象
  	  	 * 	2.创建用于接收就绪事件的epoll_event数组
  	  	 *	3.绑定ngx_event_actions变量,该变量中的方法用于操作事件驱动器中的事件
         */
        if (module->actions.init(cycle, ngx_timer_resolution) != NGX_OK) {
            /* fatal */
            exit(2);
        }

        break;
    }

#if !(NGX_WIN32)

    /**
     * ngx_timer_resolution: 对应指令timer_resolution,减少gettimeofday()方法的调用次数
     * 如果用户设置了ngx_timer_resolution值,则设置一个定时器,定时更新ngx_event_timer_alarm变量
     *
     * 基本原理是每隔一段时间调用一次ngx_timer_signal_handler方法
     * 该方法将全局变量ngx_event_timer_alarm设置为1,表示可以调用ngx_time_update方法更新缓存时间
     *
     * NGX_USE_TIMER_EVENT: eventport和kqueue模块会用到,epoll模块不会设置该标记
     * 	(The event module handles periodic or absolute timer event by itself)
     */
    if (ngx_timer_resolution && !(ngx_event_flags & NGX_USE_TIMER_EVENT)) {
        struct sigaction  sa;
        struct itimerval  itv;

        // sa对象清零
        ngx_memzero(&sa, sizeof(struct sigaction));
        // 定时器回调方法
        sa.sa_handler = ngx_timer_signal_handler;
        // 清空信号集合(不就是置零吗,搞这么大阵势)
        sigemptyset(&sa.sa_mask);

        /*
         * 设置信号SIGALRM要处理的函数
         * SIGALRM: liunx中的信号,函数alarm和setitimer设置的时钟都会产生SIGALRM信号
         */
        if (sigaction(SIGALRM, &sa, NULL) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "sigaction(SIGALRM) failed");
            return NGX_ERROR;
        }

        itv.it_interval.tv_sec = ngx_timer_resolution / 1000;
        itv.it_interval.tv_usec = (ngx_timer_resolution % 1000) * 1000;
        itv.it_value.tv_sec = ngx_timer_resolution / 1000;
        itv.it_value.tv_usec = (ngx_timer_resolution % 1000 ) * 1000;

        /*
         * 设置一个定时器,每隔itv时间就调用一次ngx_timer_signal_handler方法;
         *
         * 定时器到期后会发送SIGALRM信号,因为上面用sigaction方法将信号SIGALRM和
         * 方法ngx_timer_signal_handler绑定了,所以到期后会触发该方法。
         */
        if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "setitimer() failed");
        }
    }

    // NGX_USE_FD_EVENT: devpoll、poll这两个事件模块会用到这个标记;
    if (ngx_event_flags & NGX_USE_FD_EVENT) {
        struct rlimit  rlmt;

        // 获取当前进程文件描述符限制
        if (getrlimit(RLIMIT_NOFILE, &rlmt) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "getrlimit(RLIMIT_NOFILE) failed");
            return NGX_ERROR;
        }

        // 当前进程可以打开的最大描述符个数
        cycle->files_n = (ngx_uint_t) rlmt.rlim_cur;

        // 分配files_n个指向ngx_connection_t的指针空间
        cycle->files = ngx_calloc(sizeof(ngx_connection_t *) * cycle->files_n,
                                  cycle->log);
        if (cycle->files == NULL) {
            return NGX_ERROR;
        }
    }

#endif

    // 分配ngx_connection_t连接池内存空间,总共connection_n个
    cycle->connections =
        ngx_alloc(sizeof(ngx_connection_t) * cycle->connection_n, cycle->log);
    if (cycle->connections == NULL) {
        return NGX_ERROR;
    }

    c = cycle->connections;

    // 分配读事件池内存k空间,总共connection_n个
    cycle->read_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                   cycle->log);
    if (cycle->read_events == NULL) {
        return NGX_ERROR;
    }

    // 为所有的读事件对象初始一些标记
    rev = cycle->read_events;
    for (i = 0; i < cycle->connection_n; i++) {
    	// eventport和kqueue模块会用到
        rev[i].closed = 1;
        // 设置它是怕有脏数据吗
        rev[i].instance = 1;
    }

    // 分配写事件池,总共connection_n个
    cycle->write_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                    cycle->log);
    if (cycle->write_events == NULL) {
        return NGX_ERROR;
    }

    // 为所有写事件对象初始化closed标记
    wev = cycle->write_events;
    for (i = 0; i < cycle->connection_n; i++) {
    	// eventport和kqueue模块会用到
        wev[i].closed = 1;
    }

    i = cycle->connection_n;
    next = NULL;

    /*
     * 将读事件和写事件结构体关联到连接池中每一个连接(ngx_connection_t)上。
     *
     * 调用ngx_get_connection方法的时候才会把连接对象关联到对应的读写事件上,
     * 产生一个文件描述符后会调用ngx_get_connection方法,将文件描述符合和连接对象关联上。
     *
     */
    do {
        i--;

        // 将cycle->connections池中的连接用data指针串联起来
        c[i].data = next;
        // 将连接对象和读事件对象关联起来
        c[i].read = &cycle->read_events[i];
        // 将连接对象和写事件对象关联起来
        c[i].write = &cycle->write_events[i];
        // 连接对象对应的描述符设置为-1
        c[i].fd = (ngx_socket_t) -1;

        next = &c[i];
    } while (i);

    // 刚开始闲置连接头指向cycle->connections中的第一个连接(ngx_connection_t)
    cycle->free_connections = next;
    cycle->free_connection_n = cycle->connection_n;

    /* for each listening socket */
    /*
     * 循环cycle中的所有监听连接,并初始化监听连接
     *
     * 如果没有开启互斥锁则这个过程会把各个监听连接的读事件加入到事件驱动器中区
     */
    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

#if (NGX_HAVE_REUSEPORT)
    	// TODO reuseport(复用端口号?)
        if (ls[i].reuseport && ls[i].worker != ngx_worker) {
            continue;
        }
#endif

        /*
         * 真正获取连接的时候，才会把该连接放入到对应的读写事件结构体中
         * ls[i].fd 在ngx_init_cycle方法中已经初始化好了(设置非阻塞等)
         *
         * 根据文件描述符获取一个连接对象(ngx_connection_t)
         */
        c = ngx_get_connection(ls[i].fd, cycle->log);

        if (c == NULL) {
            return NGX_ERROR;
        }

        c->log = &ls[i].log;

        // 连接对象(ngx_connection_t)中有监听对象(ngx_listening_t)
        c->listening = &ls[i];
        // 监听对象(ngx_listening_t)中有连接对象(ngx_connection_t)
        ls[i].connection = c;

        rev = c->read;

        rev->log = c->log;
        // 读事件的accept为1表示该读事件对应的是监听连接
        rev->accept = 1;

#if (NGX_HAVE_DEFERRED_ACCEPT)
        rev->deferred_accept = ls[i].deferred_accept;
#endif

        if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) { // TODO
            if (ls[i].previous) {

                /*
                 * delete the old accept events that were bound to
                 * the old cycle read events array
                 */

                old = ls[i].previous->connection;

                if (ngx_del_event(old->read, NGX_READ_EVENT, NGX_CLOSE_EVENT)
                    == NGX_ERROR)
                {
                    return NGX_ERROR;
                }

                old->fd = (ngx_socket_t) -1;
            }
        }

#if (NGX_WIN32)

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
            ngx_iocp_conf_t  *iocpcf;

            rev->handler = ngx_event_acceptex;

            if (ngx_use_accept_mutex) {
                continue;
            }

            if (ngx_add_event(rev, 0, NGX_IOCP_ACCEPT) == NGX_ERROR) {
                return NGX_ERROR;
            }

            ls[i].log.handler = ngx_acceptex_log_error;

            iocpcf = ngx_event_get_conf(cycle->conf_ctx, ngx_iocp_module);
            if (ngx_event_post_acceptex(&ls[i], iocpcf->post_acceptex)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

        } else {
            rev->handler = ngx_event_accept;

            if (ngx_use_accept_mutex) {
                continue;
            }

            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

#else

        /*
         * 读事件发生后执行该方法,用该方法接收客户端的连接。
         * 其实该方法可以算是启动其它模块(使用了事件模块的模块)执行业务流程的一个入口。
         *
         * 因为方法ngx_event_accept最后会执行ls->handler(c)方法,而该方法则是其它模块
         * 在初始化时设置的自己的方法,比如http核心模块的ngx_http_init_connection方法。
         *
         */
        rev->handler = ngx_event_accept;

        // 检查是否使用互斥锁
        if (ngx_use_accept_mutex
#if (NGX_HAVE_REUSEPORT)
            && !ls[i].reuseport //TODO
#endif
           )
        {
        	/*
        	 * 如果使用互斥锁就不会将监听连接的读事件放入到事件驱动器中,
        	 * 这样可以保证后续worker只有在获取到互斥锁的情况下才能处理新建连接。
        	 */
            continue;
        }

        /*
         * 走到这里说明没有使用互斥锁。
         *
         * 将该监听连接的读事件加入到事件驱动器中
         */
        if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }

#endif

    }

    return NGX_OK;
}


/*
 * 在Linux系统中好像没啥用
 *
 * This directive(send_lowat) is ignored on Linux, Solaris, and Windows.
 */
ngx_int_t
ngx_send_lowat(ngx_connection_t *c, size_t lowat)
{
    int  sndlowat;

#if (NGX_HAVE_LOWAT_EVENT)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
        c->write->available = lowat;
        return NGX_OK;
    }

#endif

    // This directive is ignored on Linux, Solaris, and Windows.
    if (lowat == 0 || c->sndlowat) {
        return NGX_OK;
    }

    sndlowat = (int) lowat;

    if (setsockopt(c->fd, SOL_SOCKET, SO_SNDLOWAT,
                   (const void *) &sndlowat, sizeof(int))
        == -1)
    {
        ngx_connection_error(c, ngx_socket_errno,
                             "setsockopt(SO_SNDLOWAT) failed");
        return NGX_ERROR;
    }

    c->sndlowat = 1;

    return NGX_OK;
}


//  rv = (*cf->handler)(cf, NULL, cf->handler_conf);
/*
 * events指令对应的方法
 *
 * TODO 看完文件解析后回头再看,主要看conf
 */
static char *
ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                 *rv;
    void               ***ctx;
    ngx_uint_t            i;
    ngx_conf_t            pcf;
    ngx_event_module_t   *m;

    if (*(void **) conf) {
        return "is duplicate";
    }

    /* count the number of the event modules and set up their indices */

    // 为当前可用的事件模块设置编号
    ngx_event_max_module = 0;
    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        ngx_modules[i]->ctx_index = ngx_event_max_module++;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(void *));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *ctx = ngx_pcalloc(cf->pool, ngx_event_max_module * sizeof(void *));
    if (*ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    // 为什么做这个事???  TODO  看完配置文件的解析，回过头看再看ctx和cycle->conf_ctx是如何对应的
    // 直接 *conf=ctx 这样也行吧(是行，但是因为ctx是指针，*conf不是指针, 这样赋值不太好看)
    *(void **) conf = ctx;

    // 开始为所有事件模块创建配置信息结构体(通过m->create_conf方法创建),之后就可以取解析命令了
    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = ngx_modules[i]->ctx;

        if (m->create_conf) {
        	/* 第三层指针存放的是各个事件模块的配置项结构体,指针图形如下:
			   ctx
			   -----
			   | * | 该内存空间在变量定义时给出(void ***ctx); 值是ngx_pcalloc(cf->pool, sizeof(void *))方法的返回值。
			   -----
			   |设 *(ctx+0) 为ctx0
			   \*(ctx+0)
			   	-----
			    | * | 这一层指针有具体的核心模块负责创建。该内存空间有ngx_pcalloc(cf->pool, sizeof(void *))方法分配; 值是ngx_pcalloc(cf->pool, ngx_event_max_module * sizeof(void *))方法的返回值。
			    -----
				|设*(ctx0+0)为ctx00	|设*(ctx0+1)为ctx01
				\*(ctx0+0)   		\*(ctx0+1)
				 -----------------------------
				 |     *       |       *     | 该内存空间有ngx_pcalloc(cf->pool, ngx_event_max_module * sizeof(void *))方法分配; 值是m->create_conf(cf->cycle)方法的返回值。
				 -----------------------------
				 |					   	  |
				 \*(ctx00+0)	  		  \*(ctx01+0)
				  ------------------       ------------------
				  |ngx_event_conf_t|       |ngx_epoll_conf_t| 该内存空间有m->create_conf(cf->cycle)方法创建
				  ------------------       ------------------
        	*/
            (*ctx)[ngx_modules[i]->ctx_index] = m->create_conf(cf->cycle);
            if ((*ctx)[ngx_modules[i]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }

    // cf的结构体临时copy给pcf,用来保留现场
    pcf = *cf;
    cf->ctx = ctx;
    cf->module_type = NGX_EVENT_MODULE;
    cf->cmd_type = NGX_EVENT_CONF;

    // 解析所有事件模块的指令(例如use、worker_connections);在解析命令之前,模块对应的配置信息结构体已经创建完毕。
    rv = ngx_conf_parse(cf, NULL);

    // 复原cf指向的结构体
    *cf = pcf;

    if (rv != NGX_CONF_OK) {
        return rv;
    }

    // 调用所有可用事件模块的init_conf方法,初始化配置信息;对于事件核心模块来说就是ngx_event_core_init_conf方法
    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = ngx_modules[i]->ctx;

        if (m->init_conf) {
            rv = m->init_conf(cf->cycle, (*ctx)[ngx_modules[i]->ctx_index]);
            if (rv != NGX_CONF_OK) {
                return rv;
            }
        }
    }

    return NGX_CONF_OK;
}


/*
 * worker_connections指令对应的方法
 *
 * TODO 先看文件解析再回头看一眼
 */
static char *
ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

    ngx_str_t  *value;

    if (ecf->connections != NGX_CONF_UNSET_UINT) {
    	// 该指令不允许出现多次
        return "is duplicate";
    }

    // 获取指令值,并将其转化为int值,然后赋值给配置信息结构体
    value = cf->args->elts;
    ecf->connections = ngx_atoi(value[1].data, value[1].len);
    if (ecf->connections == (ngx_uint_t) NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number \"%V\"", &value[1]);

        return NGX_CONF_ERROR;
    }

    cf->cycle->connection_n = ecf->connections;

    return NGX_CONF_OK;
}


/*
 * use指令对应的方法
 *
 * TODO 看完文件解析再回头看一眼
 */
static char *
ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

    ngx_int_t             m;
    ngx_str_t            *value;
    ngx_event_conf_t     *old_ecf;
    ngx_event_module_t   *module;

    if (ecf->use != NGX_CONF_UNSET_UINT) {
    	// 该指令已经被设置过了,走到这里说明配置了多个use指令
        return "is duplicate";
    }

    // use指令和值(如use epoll),下标0代表指令本身,下标1代表指令值(epoll)
    value = cf->args->elts;

    // TODO
    if (cf->cycle->old_cycle->conf_ctx) {
        old_ecf = ngx_event_get_conf(cf->cycle->old_cycle->conf_ctx,
                                     ngx_event_core_module);
    } else {
        old_ecf = NULL;
    }


    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_EVENT_MODULE) {
            continue;
        }

        // 开始对比用户选择是哪个具体事件模块
        module = ngx_modules[m]->ctx;
        if (module->name->len == value[1].len) {
        	/*
        	 * 比较具体事件模块名字
        	 * epoll事件模块名字的定义:
        	 * 		static ngx_str_t epoll_name = ngx_string("epoll");
        	 *
        	 */
            if (ngx_strcmp(module->name->data, value[1].data) == 0) {

            	// 匹配成功则设置具体事件模块编号和名字
                ecf->use = ngx_modules[m]->ctx_index;
                ecf->name = module->name->data;

                if (ngx_process == NGX_PROCESS_SINGLE
                    && old_ecf
                    && old_ecf->use != ecf->use)
                {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "when the server runs without a master process "
                               "the \"%V\" event type must be the same as "
                               "in previous configuration - \"%s\" "
                               "and it cannot be changed on the fly, "
                               "to change it you need to stop server "
                               "and start it again",
                               &value[1], old_ecf->name);

                    return NGX_CONF_ERROR;
                }

                return NGX_CONF_OK;
            }
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid event type \"%V\"", &value[1]);

    return NGX_CONF_ERROR;
}


/*
 * debug_connection指令对应的方法
 */
static char *
ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
#if (NGX_DEBUG)
    ngx_event_conf_t  *ecf = conf;

    ngx_int_t             rc;
    ngx_str_t            *value;
    ngx_url_t             u;
    ngx_cidr_t            c, *cidr;
    ngx_uint_t            i;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    value = cf->args->elts;

#if (NGX_HAVE_UNIX_DOMAIN)

    if (ngx_strcmp(value[1].data, "unix:") == 0) {
        cidr = ngx_array_push(&ecf->debug_connection);
        if (cidr == NULL) {
            return NGX_CONF_ERROR;
        }

        cidr->family = AF_UNIX;
        return NGX_CONF_OK;
    }

#endif

    rc = ngx_ptocidr(&value[1], &c);

    if (rc != NGX_ERROR) {
        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "low address bits of %V are meaningless",
                               &value[1]);
        }

        cidr = ngx_array_push(&ecf->debug_connection);
        if (cidr == NULL) {
            return NGX_CONF_ERROR;
        }

        *cidr = c;

        return NGX_CONF_OK;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));
    u.host = value[1];

    if (ngx_inet_resolve_host(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "%s in debug_connection \"%V\"",
                               u.err, &u.host);
        }

        return NGX_CONF_ERROR;
    }

    cidr = ngx_array_push_n(&ecf->debug_connection, u.naddrs);
    if (cidr == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(cidr, u.naddrs * sizeof(ngx_cidr_t));

    for (i = 0; i < u.naddrs; i++) {
        cidr[i].family = u.addrs[i].sockaddr->sa_family;

        switch (cidr[i].family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) u.addrs[i].sockaddr;
            cidr[i].u.in6.addr = sin6->sin6_addr;
            ngx_memset(cidr[i].u.in6.mask.s6_addr, 0xff, 16);
            break;
#endif

        default: /* AF_INET */
            sin = (struct sockaddr_in *) u.addrs[i].sockaddr;
            cidr[i].u.in.addr = sin->sin_addr.s_addr;
            cidr[i].u.in.mask = 0xffffffff;
            break;
        }
    }

#else

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "\"debug_connection\" is ignored, you need to rebuild "
                       "nginx using --with-debug option to enable it");

#endif

    return NGX_CONF_OK;
}


/**
 * 创建用来存放事件核心模块配置信息的结构体
 * (此时指令还没有开始解析,等创建了存储指令信息的结构体才能解析指令)
 */
static void *
ngx_event_core_create_conf(ngx_cycle_t *cycle)
{
    ngx_event_conf_t  *ecf;

    ecf = ngx_palloc(cycle->pool, sizeof(ngx_event_conf_t));
    if (ecf == NULL) {
        return NULL;
    }

    // 为下面的指令设置初始值
    ecf->connections = NGX_CONF_UNSET_UINT;
    ecf->use = NGX_CONF_UNSET_UINT;
    ecf->multi_accept = NGX_CONF_UNSET;
    ecf->accept_mutex = NGX_CONF_UNSET;
    ecf->accept_mutex_delay = NGX_CONF_UNSET_MSEC;
    ecf->name = (void *) NGX_CONF_UNSET;

#if (NGX_DEBUG)

    if (ngx_array_init(&ecf->debug_connection, cycle->pool, 4,
                       sizeof(ngx_cidr_t)) == NGX_ERROR)
    {
        return NULL;
    }

#endif

    return ecf;
}


/**
 * 初始化事件核心模块的配置信息
 * (此时指令都已经解析完毕)
 */
static char *
ngx_event_core_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

#if (NGX_HAVE_EPOLL) && !(NGX_TEST_BUILD_EPOLL)
    int                  fd;
#endif
    ngx_int_t            i;
    ngx_module_t        *module;
    ngx_event_module_t  *event_module;

    module = NULL;

    // NGX_HAVE_EPOLL如果epoll可用,则该宏定义在 /objs/ngx_auto_config.h 中出现
#if (NGX_HAVE_EPOLL) && !(NGX_TEST_BUILD_EPOLL)

    /*
     * 如果ngx在config的时候定义了宏NGX_HAVE_EPOLL,这里会测试一下epoll是否可用
     * 可用则moudle指向ngx_epoll_module变量
     *
     * 不可用则继续向下走
     */

    fd = epoll_create(100);

    if (fd != -1) {
        (void) close(fd);
        module = &ngx_epoll_module;

    } else if (ngx_errno != NGX_ENOSYS) {
        module = &ngx_epoll_module;
    }

#endif

    // 同NGX_HAVE_EPOLL宏
#if (NGX_HAVE_DEVPOLL)

    module = &ngx_devpoll_module;

#endif

    // 同NGX_HAVE_EPOLL宏
#if (NGX_HAVE_KQUEUE)

    module = &ngx_kqueue_module;

#endif

    // 同NGX_HAVE_EPOLL宏
#if (NGX_HAVE_SELECT)

    if (module == NULL) {
    	// 如果所有的高级事件驱动器都不支持则使用select

        module = &ngx_select_module;
    }

#endif

    if (module == NULL) {

    	/**
    	 * 如果所有的宏定义都不存在,则从ngx_modules数组中选择第一个具体事件模块
    	 */

        for (i = 0; ngx_modules[i]; i++) {

            if (ngx_modules[i]->type != NGX_EVENT_MODULE) {
                continue;
            }

            event_module = ngx_modules[i]->ctx;

            if (ngx_strcmp(event_module->name->data, event_core_name.data) == 0)
            {
            	// 将事件核心模块排除掉,因为他不负责处理具体事件
                continue;
            }

            module = ngx_modules[i];
            break;
        }
    }

    if (module == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0, "no events module found");
        return NGX_CONF_ERROR;
    }

    ngx_conf_init_uint_value(ecf->connections, DEFAULT_CONNECTIONS);
    cycle->connection_n = ecf->connections;

    // 只有ecf->use为NGX_CONF_UNSET_UINT时该方法才会起作用,也就是说,如果存在use指令,则该方法就不会执行
    ngx_conf_init_uint_value(ecf->use, module->ctx_index);

    event_module = module->ctx;
    // 只有ecf->name为NGX_CONF_UNSET_PTR时该方法才会起作用
    ngx_conf_init_ptr_value(ecf->name, event_module->name->data);

    // 只有ecf->multi_accept为NGX_CONF_UNSET时该方法才会起作用
    ngx_conf_init_value(ecf->multi_accept, 0);
    // 默认开启互斥锁
    ngx_conf_init_value(ecf->accept_mutex, 1);
    ngx_conf_init_msec_value(ecf->accept_mutex_delay, 500);

    return NGX_CONF_OK;
}
