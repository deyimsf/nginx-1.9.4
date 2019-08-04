
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * 日志的打印级别使用error_log指令设置,如:
 *    error_log logs/error.log info;
 *
 * 日志由两种宏定义
 *    ngx_log_error(level,log,args, ...)
 *    ngx_log_debugX(level,log,args, ...)
 *
 * 当log->log_level >= level时ngx_log_error()宏才会生效
 *    if ((log)->log_level >= level) ngx_log_error_core(level, log, args)
 *
 * 当log->log_level & level为true时ngx_log_debugX()宏才会生效
 *    if ((log)->log_level & level) ngx_log_error_core(NGX_LOG_DEBUG, log, args)
 *
 *
 * 为什么针对ngx_log_debugX()宏是否生效要用与操作呢?
 * 目前在ngx中有八种类型的debug信息,分别是:
 *    NGX_LOG_DEBUG_CORE        0x010    debug_core
 *    NGX_LOG_DEBUG_ALLOC       0x020    debug_alloc
 *    NGX_LOG_DEBUG_MUTEX       0x040    debug_mutex
 *    NGX_LOG_DEBUG_EVENT       0x080    debug_event
 *    NGX_LOG_DEBUG_HTTP        0x100    debug_http
 *    NGX_LOG_DEBUG_MAIL        0x200    debug_mail
 *    NGX_LOG_DEBUG_MYSQL       0x400    debug_mysql
 *    NGX_LOG_DEBUG_STREAM      0x800    debug_stream
 * 调问题的时候可能不需要看所有的debug信息,比如只想看事件模块相关的debug信息,做如下配置
 *    error_log logs/error.log debug_event;
 * 此时用与操作来生效特定的debug信息
 *
 * 当把error_log指令的日志打印级别设置为debug时会打印所有debug信息,但是这里又有一个问题,ngx中debug对应的宏是
 *    NGX_LOG_DEBUG    8
 * 很明显,数字8个目前和任何一种特定debug信息做与操作都是假,那为什么这个宏又能起作用呢?
 * 实际上是error_log指令对参数是debug时做了一个特殊处理,处理过程如下:
 *    if (log->log_level == NGX_LOG_DEBUG) {
 *        log->log_level = NGX_LOG_DEBUG_ALL;
 *    }
 * 而NGX_LOG_DEBUG_ALL对应值 0x7ffffff0,他包含了所有特定debug值
 *
 *
 * 日志各种格式看 string.c
 */

#include <ngx_config.h>
#include <ngx_core.h>


static char *ngx_error_log(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_log_set_levels(ngx_conf_t *cf, ngx_log_t *log);
static void ngx_log_insert(ngx_log_t *log, ngx_log_t *new_log);


#if (NGX_DEBUG)

static void ngx_log_memory_writer(ngx_log_t *log, ngx_uint_t level,
    u_char *buf, size_t len);
static void ngx_log_memory_cleanup(void *data);


typedef struct {
    u_char        *start;
    u_char        *end;
    u_char        *pos;
    ngx_atomic_t   written;
} ngx_log_memory_buf_t;

#endif


static ngx_command_t  ngx_errlog_commands[] = {

    {ngx_string("error_log"),
     NGX_MAIN_CONF|NGX_CONF_1MORE,
     ngx_error_log,
     0,
     0,
     NULL},

    ngx_null_command
};


static ngx_core_module_t  ngx_errlog_module_ctx = {
    ngx_string("errlog"),
    NULL,
    NULL
};


ngx_module_t  ngx_errlog_module = {
    NGX_MODULE_V1,
    &ngx_errlog_module_ctx,                /* module context */
    ngx_errlog_commands,                   /* module directives */
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


static ngx_log_t        ngx_log;
static ngx_open_file_t  ngx_log_file;
ngx_uint_t              ngx_use_stderr = 1;


/*
 * 日志级别对应的串,比如
 *      error_log logs/error.log warn;
 */
static ngx_str_t err_levels[] = {
    ngx_null_string,
    ngx_string("emerg"),
    ngx_string("alert"),
    ngx_string("crit"),
    ngx_string("error"),
    ngx_string("warn"),
    ngx_string("notice"),
    ngx_string("info"),
    ngx_string("debug")
};

/*
 * 同err_levels[],专门为debug准备的日志级别,比如
 *       error_log logs/error.log debug_core;
 * 上面的配置表示只打印dubug_core级别的debug日志,也就是如下级别的debug日志
 *       ngx_log_debugX(NGX_LOG_DEBUG_CORE, c->log, ...);
 * 其它级别的debug日志是不会打印的
 */
static const char *debug_levels[] = {
    "debug_core", "debug_alloc", "debug_mutex", "debug_event",
    "debug_http", "debug_mail", "debug_mysql", "debug_stream"
};


#if (NGX_HAVE_VARIADIC_MACROS)

void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)

#else

/*
 * linux
 *
 */
void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, va_list args)

#endif
{
#if (NGX_HAVE_VARIADIC_MACROS)
    va_list      args;
#endif
    u_char      *p, *last, *msg;
    ssize_t      n;
    ngx_uint_t   wrote_stderr, debug_connection;

    /* 暂时用来存放日志信息的内存空间,局部变量所以是栈上分配 */
    u_char       errstr[NGX_MAX_ERROR_STR];

    /*
     * 暂时用来缓存日志信息的内存的最后边界地址
     */
    last = errstr + NGX_MAX_ERROR_STR;

    /*
     * 把代表当前时间的字符串拷贝到errstr中,然后返回这个时间串在errstr内存中最后一个字符的后面的地址
     * 假设ngx_cached_err_log_time的值是:
     *        2018/01/14 08:36:47
     * 执行完下面的拷贝后errstr的内存结构如下
     *       errstr
     *       -----
     *       | * |
     *       -----
     *           \          11      18
     *          ------------------------------
     *          |2018/01/14 08:36:47 |
     *          ------------------------------
     * 日期最后一个字符的地址是17,那么p就是18
     *
     */
    p = ngx_cpymem(errstr, ngx_cached_err_log_time.data,
                   ngx_cached_err_log_time.len);

    /*
     * 把错误级别对应的字符串拷贝给errstr中,从p这个位置开始拷贝,拷贝后内存结构如下,假设错误级别是error
     *      errstr
     *      -----
     *      | * |
     *      -----
     *         \                   18
     *         ----------------------------------
     *         |2018/01/14 08:36:47 [error]
     *         ----------------------------------
     *
     */
    p = ngx_slprintf(p, last, " [%V] ", &err_levels[level]);

    /*
     * 拷贝进程号和线程好,如果没有用线程则线程号用0代替,拷贝后内存结构如下
     *      errstr
     *      -----
     *      | * |
     *      -----
     *          \                  18
     *         ----------------------------------------
     *         |2018/01/14 08:36:47 [error] 2074#0:
     *         ----------------------------------------
     *
     */
    p = ngx_slprintf(p, last, "%P#" NGX_TID_T_FMT ": ",
                    ngx_log_pid, ngx_log_tid);

    if (log->connection) {
        /*
         * 这个号是干嘛滴,拷贝后如下:
         *      ------------------------------------------
         *      |2018/01/14 08:36:47 [error] 2074#0: *98
         *      ------------------------------------------
         *
         */

        p = ngx_slprintf(p, last, "*%uA ", log->connection);
    }

    msg = p;

#if (NGX_HAVE_VARIADIC_MACROS)

    /*
     * 下面开始真正处理用户穿过来的格式化字符串,比如:
     *     ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "no live upstreams");
     * 那么此时fmt就是"no live upstreams"
     *
     * 可以看到这个字符串并没有一个百分号,所以下面的方法执行完毕后结果如下:
     *     -----------------------------------------------------------------
     *     |2018/01/14 08:36:47 [error] 2074#0: *98 no live upstreams
     *     -----------------------------------------------------------------
     *
     *
     */

    va_start(args, fmt);
    p = ngx_vslprintf(p, last, fmt, args);
    va_end(args);

#else

    p = ngx_vslprintf(p, last, fmt, args);

#endif

    if (err) {
        p = ngx_log_errno(p, last, err);
    }

    if (level != NGX_LOG_DEBUG && log->handler) {
        /*
         * 回调handler方法,对于http模块来说,这个handler就是ngx_http_log_error()方法
         * 该方法的作用是向p这个指针地址中拷贝不大于last-p个字节的字符
         *
         * 比如在ngx_http_log_error()方法中当log->action中有值的时候,会把log中的值拷贝到p中
         * 假设log->action = "connecting to upstream",那么执行过程会把log->action 中的值打印到p中:
         *    -------------------------------------------------------------------------------------------
         *    |2018/01/14 08:36:47 [error] 2074#0: *98 no live upstreams  while connecting to upstream
         *    -------------------------------------------------------------------------------------------
         * 然后还会打印客户端ip和server端ip(ngx本地ip)
         *  , client: 127.0.0.1 , server: localhost,
         *
         * 其它信息等
         *  , request: "GET /init HTTP/1.1", upstream: "http://tomcat1/init", host: "127.0.0.1"
         *
         * 如果日志级别是DEBUG的时候就不会回调该方法了
         */

        p = log->handler(log, p, last - p);
    }

    if (p > last - NGX_LINEFEED_SIZE) {
        /*
         * 最终要输出的字符串长度不能大于last加一个换行的长度
         */
        p = last - NGX_LINEFEED_SIZE;
    }

    /*
     * 增加一个换行符
     */
    ngx_linefeed(p);

    wrote_stderr = 0;
    debug_connection = (log->log_level & NGX_LOG_DEBUG_CONNECTION) != 0;

    /*
     * 将收集好的日志信息通过log链表打印出去
     */
    while (log) {

        if (log->log_level < level && !debug_connection) {
            break;
        }

        if (log->writer) {
            log->writer(log, level, errstr, p - errstr);
            goto next;
        }

        if (ngx_time() == log->disk_full_time) {

            /*
             * on FreeBSD writing to a full filesystem with enabled softupdates
             * may block process for much longer time than writing to non-full
             * filesystem, so we skip writing to a log for one second
             */

            goto next;
        }

        n = ngx_write_fd(log->file->fd, errstr, p - errstr);

        if (n == -1 && ngx_errno == NGX_ENOSPC) {
            /*
             * write()函数返回-1表示发生错误
             * errno为ENOSPC表示磁盘满了
             *
             * TODO 一个疑问,如果一次输出不完errstr中的数据怎么办?就不输出了?还是说根本不会发生这种情况?
             */
            log->disk_full_time = ngx_time();
        }

        if (log->file->fd == ngx_stderr) {
            wrote_stderr = 1;
        }

    next:

        log = log->next;
    }

    if (!ngx_use_stderr
        || level > NGX_LOG_WARN
        || wrote_stderr)
    {
        return;
    }

    /*
     * 出错了,或者当前要打印的日志错误级别严重于WARN(比如ERR),则追加一些信息,并把追加的错误信息打印到控制台
     */

    msg -= (7 + err_levels[level].len + 3);

    (void) ngx_sprintf(msg, "nginx: [%V] ", &err_levels[level]);

    (void) ngx_write_console(ngx_stderr, msg, p - msg);
}


#if !(NGX_HAVE_VARIADIC_MACROS)

void ngx_cdecl
ngx_log_error(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{
    va_list  args;

    if (log->log_level >= level) {
        va_start(args, fmt);
        ngx_log_error_core(level, log, err, fmt, args);
        va_end(args);
    }
}


void ngx_cdecl
ngx_log_debug_core(ngx_log_t *log, ngx_err_t err, const char *fmt, ...)
{
    va_list  args;

    va_start(args, fmt);
    ngx_log_error_core(NGX_LOG_DEBUG, log, err, fmt, args);
    va_end(args);
}

#endif


void ngx_cdecl
ngx_log_abort(ngx_err_t err, const char *fmt, ...)
{
    u_char   *p;
    va_list   args;
    u_char    errstr[NGX_MAX_CONF_ERRSTR];

    va_start(args, fmt);
    p = ngx_vsnprintf(errstr, sizeof(errstr) - 1, fmt, args);
    va_end(args);

    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err,
                  "%*s", p - errstr, errstr);
}


void ngx_cdecl
ngx_log_stderr(ngx_err_t err, const char *fmt, ...)
{
    u_char   *p, *last;
    va_list   args;
    u_char    errstr[NGX_MAX_ERROR_STR];

    last = errstr + NGX_MAX_ERROR_STR;

    p = ngx_cpymem(errstr, "nginx: ", 7);

    va_start(args, fmt);
    p = ngx_vslprintf(p, last, fmt, args);
    va_end(args);

    if (err) {
        p = ngx_log_errno(p, last, err);
    }

    if (p > last - NGX_LINEFEED_SIZE) {
        p = last - NGX_LINEFEED_SIZE;
    }

    ngx_linefeed(p);

    (void) ngx_write_console(ngx_stderr, errstr, p - errstr);
}


u_char *
ngx_log_errno(u_char *buf, u_char *last, ngx_err_t err)
{
    if (buf > last - 50) {

        /* leave a space for an error code */

        buf = last - 50;
        *buf++ = '.';
        *buf++ = '.';
        *buf++ = '.';
    }

#if (NGX_WIN32)
    buf = ngx_slprintf(buf, last, ((unsigned) err < 0x80000000)
                                       ? " (%d: " : " (%Xd: ", err);
#else
    buf = ngx_slprintf(buf, last, " (%d: ", err);
#endif

    buf = ngx_strerror(err, buf, last - buf);

    if (buf < last) {
        *buf++ = ')';
    }

    return buf;
}


ngx_log_t *
ngx_log_init(u_char *prefix)
{
    u_char  *p, *name;
    size_t   nlen, plen;

    ngx_log.file = &ngx_log_file;
    ngx_log.log_level = NGX_LOG_NOTICE;

    name = (u_char *) NGX_ERROR_LOG_PATH;

    /*
     * we use ngx_strlen() here since BCC warns about
     * condition is always false and unreachable code
     */

    nlen = ngx_strlen(name);

    if (nlen == 0) {
        ngx_log_file.fd = ngx_stderr;
        return &ngx_log;
    }

    p = NULL;

#if (NGX_WIN32)
    if (name[1] != ':') {
#else
    if (name[0] != '/') {
#endif

        if (prefix) {
            plen = ngx_strlen(prefix);

        } else {
#ifdef NGX_PREFIX
            prefix = (u_char *) NGX_PREFIX;
            plen = ngx_strlen(prefix);
#else
            plen = 0;
#endif
        }

        if (plen) {
            name = malloc(plen + nlen + 2);
            if (name == NULL) {
                return NULL;
            }

            p = ngx_cpymem(name, prefix, plen);

            if (!ngx_path_separator(*(p - 1))) {
                *p++ = '/';
            }

            ngx_cpystrn(p, (u_char *) NGX_ERROR_LOG_PATH, nlen + 1);

            p = name;
        }
    }

    ngx_log_file.fd = ngx_open_file(name, NGX_FILE_APPEND,
                                    NGX_FILE_CREATE_OR_OPEN,
                                    NGX_FILE_DEFAULT_ACCESS);

    if (ngx_log_file.fd == NGX_INVALID_FILE) {
        ngx_log_stderr(ngx_errno,
                       "[alert] could not open error log file: "
                       ngx_open_file_n " \"%s\" failed", name);
#if (NGX_WIN32)
        ngx_event_log(ngx_errno,
                       "could not open error log file: "
                       ngx_open_file_n " \"%s\" failed", name);
#endif

        ngx_log_file.fd = ngx_stderr;
    }

    if (p) {
        ngx_free(p);
    }

    return &ngx_log;
}


ngx_int_t
ngx_log_open_default(ngx_cycle_t *cycle)
{
    ngx_log_t         *log;
    static ngx_str_t   error_log = ngx_string(NGX_ERROR_LOG_PATH);

    if (ngx_log_get_file_log(&cycle->new_log) != NULL) {
        return NGX_OK;
    }

    if (cycle->new_log.log_level != 0) {
        /* there are some error logs, but no files */

        log = ngx_pcalloc(cycle->pool, sizeof(ngx_log_t));
        if (log == NULL) {
            return NGX_ERROR;
        }

    } else {
        /* no error logs at all */
        log = &cycle->new_log;
    }

    log->log_level = NGX_LOG_ERR;

    log->file = ngx_conf_open_file(cycle, &error_log);
    if (log->file == NULL) {
        return NGX_ERROR;
    }

    if (log != &cycle->new_log) {
        ngx_log_insert(&cycle->new_log, log);
    }

    return NGX_OK;
}


/*
 * 将标准错误输出定位到log指定的文件描述符中
 */
ngx_int_t
ngx_log_redirect_stderr(ngx_cycle_t *cycle)
{
    ngx_fd_t  fd;

    if (cycle->log_use_stderr) {
        return NGX_OK;
    }

    /* file log always exists when we are called */
    // 获取/export/servers/nginx/logs/error.log文件的描述符
    fd = ngx_log_get_file_log(cycle->log)->file->fd;

    if (fd != ngx_stderr) {
        // 调用dup2方法,将标准错误输出描述符定位到fd这个文件描述符中
        if (ngx_set_stderr(fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          ngx_set_stderr_n " failed");

            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


ngx_log_t *
ngx_log_get_file_log(ngx_log_t *head)
{
    ngx_log_t  *log;

    for (log = head; log; log = log->next) {
        if (log->file != NULL) {
            return log;
        }
    }

    return NULL;
}


static char *
ngx_log_set_levels(ngx_conf_t *cf, ngx_log_t *log)
{
    ngx_uint_t   i, n, d, found;
    ngx_str_t   *value;

    if (cf->args->nelts == 2) {
        log->log_level = NGX_LOG_ERR;
        return NGX_CONF_OK;
    }

    value = cf->args->elts;

    for (i = 2; i < cf->args->nelts; i++) {
        found = 0;

        for (n = 1; n <= NGX_LOG_DEBUG; n++) {
            if (ngx_strcmp(value[i].data, err_levels[n].data) == 0) {

                if (log->log_level != 0) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "duplicate log level \"%V\"",
                                       &value[i]);
                    return NGX_CONF_ERROR;
                }

                log->log_level = n;
                found = 1;
                break;
            }
        }

        for (n = 0, d = NGX_LOG_DEBUG_FIRST; d <= NGX_LOG_DEBUG_LAST; d <<= 1) {
            if (ngx_strcmp(value[i].data, debug_levels[n++]) == 0) {
                if (log->log_level & ~NGX_LOG_DEBUG_ALL) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "invalid log level \"%V\"",
                                       &value[i]);
                    return NGX_CONF_ERROR;
                }

                log->log_level |= d;
                found = 1;
                break;
            }
        }


        if (!found) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid log level \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }
    }

    if (log->log_level == NGX_LOG_DEBUG) {
        log->log_level = NGX_LOG_DEBUG_ALL;
    }

    return NGX_CONF_OK;
}


static char *
ngx_error_log(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_log_t  *dummy;

    dummy = &cf->cycle->new_log;

    return ngx_log_set_log(cf, &dummy);
}


char *
ngx_log_set_log(ngx_conf_t *cf, ngx_log_t **head)
{
    ngx_log_t          *new_log;
    ngx_str_t          *value, name;
    ngx_syslog_peer_t  *peer;

    if (*head != NULL && (*head)->log_level == 0) {
        new_log = *head;

    } else {

        new_log = ngx_pcalloc(cf->pool, sizeof(ngx_log_t));
        if (new_log == NULL) {
            return NGX_CONF_ERROR;
        }

        if (*head == NULL) {
            *head = new_log;
        }
    }

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "stderr") == 0) {
        ngx_str_null(&name);
        cf->cycle->log_use_stderr = 1;

        new_log->file = ngx_conf_open_file(cf->cycle, &name);
        if (new_log->file == NULL) {
            return NGX_CONF_ERROR;
        }

     } else if (ngx_strncmp(value[1].data, "memory:", 7) == 0) {

#if (NGX_DEBUG)
        size_t                 size, needed;
        ngx_pool_cleanup_t    *cln;
        ngx_log_memory_buf_t  *buf;

        value[1].len -= 7;
        value[1].data += 7;

        needed = sizeof("MEMLOG  :" NGX_LINEFEED)
                 + cf->conf_file->file.name.len
                 + NGX_SIZE_T_LEN
                 + NGX_INT_T_LEN
                 + NGX_MAX_ERROR_STR;

        size = ngx_parse_size(&value[1]);

        if (size == (size_t) NGX_ERROR || size < needed) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid buffer size \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }

        buf = ngx_pcalloc(cf->pool, sizeof(ngx_log_memory_buf_t));
        if (buf == NULL) {
            return NGX_CONF_ERROR;
        }

        buf->start = ngx_pnalloc(cf->pool, size);
        if (buf->start == NULL) {
            return NGX_CONF_ERROR;
        }

        buf->end = buf->start + size;

        buf->pos = ngx_slprintf(buf->start, buf->end, "MEMLOG %uz %V:%ui%N",
                                size, &cf->conf_file->file.name,
                                cf->conf_file->line);

        ngx_memset(buf->pos, ' ', buf->end - buf->pos);

        cln = ngx_pool_cleanup_add(cf->pool, 0);
        if (cln == NULL) {
            return NGX_CONF_ERROR;
        }

        cln->data = new_log;
        cln->handler = ngx_log_memory_cleanup;

        new_log->writer = ngx_log_memory_writer;
        new_log->wdata = buf;

#else
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "nginx was built without debug support");
        return NGX_CONF_ERROR;
#endif

     } else if (ngx_strncmp(value[1].data, "syslog:", 7) == 0) {
        peer = ngx_pcalloc(cf->pool, sizeof(ngx_syslog_peer_t));
        if (peer == NULL) {
            return NGX_CONF_ERROR;
        }

        if (ngx_syslog_process_conf(cf, peer) != NGX_CONF_OK) {
            return NGX_CONF_ERROR;
        }

        new_log->writer = ngx_syslog_writer;
        new_log->wdata = peer;

    } else {
        new_log->file = ngx_conf_open_file(cf->cycle, &value[1]);
        if (new_log->file == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    if (ngx_log_set_levels(cf, new_log) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }

    if (*head != new_log) {
        ngx_log_insert(*head, new_log);
    }

    return NGX_CONF_OK;
}


static void
ngx_log_insert(ngx_log_t *log, ngx_log_t *new_log)
{
    ngx_log_t  tmp;

    if (new_log->log_level > log->log_level) {

        /*
         * list head address is permanent, insert new log after
         * head and swap its contents with head
         */

        tmp = *log;
        *log = *new_log;
        *new_log = tmp;

        log->next = new_log;
        return;
    }

    while (log->next) {
        if (new_log->log_level > log->next->log_level) {
            new_log->next = log->next;
            log->next = new_log;
            return;
        }

        log = log->next;
    }

    log->next = new_log;
}


#if (NGX_DEBUG)

static void
ngx_log_memory_writer(ngx_log_t *log, ngx_uint_t level, u_char *buf,
    size_t len)
{
    u_char                *p;
    size_t                 avail, written;
    ngx_log_memory_buf_t  *mem;

    mem = log->wdata;

    if (mem == NULL) {
        return;
    }

    written = ngx_atomic_fetch_add(&mem->written, len);

    p = mem->pos + written % (mem->end - mem->pos);

    avail = mem->end - p;

    if (avail >= len) {
        ngx_memcpy(p, buf, len);

    } else {
        ngx_memcpy(p, buf, avail);
        ngx_memcpy(mem->pos, buf + avail, len - avail);
    }
}


static void
ngx_log_memory_cleanup(void *data)
{
    ngx_log_t *log = data;

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, log, 0, "destroy memory log buffer");

    log->wdata = NULL;
}

#endif
