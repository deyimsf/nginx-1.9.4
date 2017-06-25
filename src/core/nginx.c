
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/**
 * ngx是一个模块化的软件架构,他将模块抽象化为ngx_moudle_t结构体。
 *
 * 几乎所有的主要功能都是围绕着模块来做的,模块可以定义成不同的类型,
 * 如:NGX_CORE_MODULE(核心模块)、NGX_HTTP_MODULE(http模块).
 *
 * 每种模块类型都可以定义他自己的一些行为规范,比如核心模块使用ngx_core_module_t
 * 结构体来制定所以核心模块需要遵守的规则;http模块使用ngx_http_module_t结构体
 * 来指定所有http模块需要遵守的规则;
 *
 * 非要和面向对象语言对比的话,ngx_moudle_t像是超级抽象类,ngx_core_module_t、ngx_http_module_t等
 * 就想是继承了超类的子类.
 *
 *
 * ngx中每个模块都有一个结构体,这个结构体用来保存该模块的一些配置信息,我们管他叫做配置信息结构体,
 * 具体每个模块需要创建多少个配置信息结构体则由他们的管理模块来决定。
 * 	 核心主模块(ngx_core_module)只需要一个ngx_core_conf_t配置信息结构体
 *
 *	 事件核心模块(ngx_event_core_module)只需要一个ngx_event_conf_t配置信息结构体
 *
 *	 核心http模块(ngx_http_module)需要一个ngx_http_conf_ctx_t结构体
 *
 *	 http核心模块(ngx_http_core_module)多个ngx_http_conf_ctx_t结构体,具体多少个取决于
 *	 有多少个server{}指令块和多少个location{}指令块
 *
 *	 在核心http模块(ngx_http_module)的ngx_http_block方法中,会调用所有http模块的
 *	 create_main_conf、create_srv_conf、create_loc_conf这三个方法。
 *	 在http核心模块(ngx_http_core_module)的ngx_http_core_server方法中,会再次调用所有http模块的
 *	 create_srv_conf、create_loc_conf这两个方法。
 *   在http核心模块(ngx_http_core_module)的ngx_http_core_location方法中,会再次调用所有http模块
 *   的create_loc_conf方法。
 *
 *
 *
 *
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <nginx.h>


static ngx_int_t ngx_add_inherited_sockets(ngx_cycle_t *cycle);
static ngx_int_t ngx_get_options(int argc, char *const *argv);
static ngx_int_t ngx_process_options(ngx_cycle_t *cycle);
static ngx_int_t ngx_save_argv(ngx_cycle_t *cycle, int argc, char *const *argv);
static void *ngx_core_module_create_conf(ngx_cycle_t *cycle);
static char *ngx_core_module_init_conf(ngx_cycle_t *cycle, void *conf);
static char *ngx_set_user(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_set_env(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_set_priority(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_set_cpu_affinity(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_set_worker_processes(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_conf_enum_t  ngx_debug_points[] = {
    { ngx_string("stop"), NGX_DEBUG_POINTS_STOP },
    { ngx_string("abort"), NGX_DEBUG_POINTS_ABORT },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_core_commands[] = {

    { ngx_string("daemon"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_core_conf_t, daemon),
      NULL },

    { ngx_string("master_process"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_core_conf_t, master),
      NULL },

    { ngx_string("timer_resolution"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_core_conf_t, timer_resolution),
      NULL },

    { ngx_string("pid"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      0,
      offsetof(ngx_core_conf_t, pid),
      NULL },

    { ngx_string("lock_file"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      0,
      offsetof(ngx_core_conf_t, lock_file),
      NULL },

    { ngx_string("worker_processes"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_set_worker_processes,
      0,
      0,
      NULL },

    { ngx_string("debug_points"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      0,
      offsetof(ngx_core_conf_t, debug_points),
      &ngx_debug_points },

    { ngx_string("user"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE12,
      ngx_set_user,
      0,
      0,
      NULL },

    { ngx_string("worker_priority"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_set_priority,
      0,
      0,
      NULL },

    { ngx_string("worker_cpu_affinity"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_1MORE,
      ngx_set_cpu_affinity,
      0,
      0,
      NULL },

    { ngx_string("worker_rlimit_nofile"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_core_conf_t, rlimit_nofile),
      NULL },

    { ngx_string("worker_rlimit_core"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_off_slot,
      0,
      offsetof(ngx_core_conf_t, rlimit_core),
      NULL },

    { ngx_string("working_directory"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      0,
      offsetof(ngx_core_conf_t, working_directory),
      NULL },

    { ngx_string("env"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_set_env,
      0,
      0,
      NULL },

      ngx_null_command
};


/**
 * 声明一个核心模块上下文,这个核心模块的上下文叫core
 *
 */
static ngx_core_module_t  ngx_core_module_ctx = {
	// 模块名字
    ngx_string("core"),
    ngx_core_module_create_conf,
    ngx_core_module_init_conf
};


/**
 * 声明一个模块,这个模块是一个核心模块
 *
 * 这个核心模块用来规定ngx的运行时的一些基本行为，比如worker个数
 *
 */
ngx_module_t  ngx_core_module = {
    NGX_MODULE_V1,
    &ngx_core_module_ctx,                  /* module context */
    ngx_core_commands,                     /* module directives */
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


ngx_uint_t          ngx_max_module;

static ngx_uint_t   ngx_show_help;
static ngx_uint_t   ngx_show_version;
static ngx_uint_t   ngx_show_configure;
static u_char      *ngx_prefix;
static u_char      *ngx_conf_file;
static u_char      *ngx_conf_params;
// 存储命令参数如reload、stop等
static char        *ngx_signal;

// 环境变量,指向environ(内置全局环境变量)
static char **ngx_os_environ;


/**
 * nginx启动入口
 *
 * argc: 入参个数
 * argv: 入参，可以是多个字符串,比如：
 * 		name value flag等
 *
 * 		用const修饰 *argv表示传入的字符串地址不可变
 * 		   	   argv
 * 		   	   -------
 * 			   |  *	 |
 * 			   -------
 * 		         |argv0 	| argv1    | argv2
 * 		         \          \		   \
 *			      ----------------------------------------
 *			      |  *       |    *     |    *    |
 *			      ----------------------------------------
 *				     |		     |         |
 *				     \		     \		   \
 *				      ------      -------   --------
 *				      |name|      |value|   | falg |
 *				      ------      -------   --------
 */
int ngx_cdecl
main(int argc, char *const *argv)
{
    ngx_buf_t        *b;
    ngx_log_t        *log;
    ngx_uint_t        i;
    ngx_cycle_t      *cycle, init_cycle;
    ngx_conf_dump_t  *cd;
    ngx_core_conf_t  *ccf;

    ngx_debug_init();

    /*
     *  初始化数组ngx_sys_errlist
     *  把系统错误码全部拷贝到ngx_sys_errlist数组中
     */
    if (ngx_strerror_init() != NGX_OK) {
        return 1;
    }

    /*
     * 解析启动nginx时指定的入参,也就是获取用户输入的命令
     * 解析出来后会放入到静态变量中,比如ngx_show_version变量
     * 比如ngx_process:当前是什么流程
     */
    if (ngx_get_options(argc, argv) != NGX_OK) {
        return 1;
    }

    /*
     *  凡是展示的信息都会先显示版本号, 如ngx_show_help和ngx_show_configure
     *  显示版本信息。
     */
    if (ngx_show_version) {
        ngx_write_stderr("nginx version: " NGINX_VER_BUILD NGX_LINEFEED);

        // 打印帮助信息
        if (ngx_show_help) {
            ngx_write_stderr(
                "Usage: nginx [-?hvVtTq] [-s signal] [-c filename] "
                             "[-p prefix] [-g directives]" NGX_LINEFEED
                             NGX_LINEFEED
                "Options:" NGX_LINEFEED
                "  -?,-h         : this help" NGX_LINEFEED
                "  -v            : show version and exit" NGX_LINEFEED
                "  -V            : show version and configure options then exit"
                                   NGX_LINEFEED
                "  -t            : test configuration and exit" NGX_LINEFEED
                "  -T            : test configuration, dump it and exit"
                                   NGX_LINEFEED
                "  -q            : suppress non-error messages "
                                   "during configuration testing" NGX_LINEFEED
                "  -s signal     : send signal to a master process: "
                                   "stop, quit, reopen, reload" NGX_LINEFEED
#ifdef NGX_PREFIX
                "  -p prefix     : set prefix path (default: "
                                   NGX_PREFIX ")" NGX_LINEFEED
#else
                "  -p prefix     : set prefix path (default: NONE)" NGX_LINEFEED
#endif
                "  -c filename   : set configuration file (default: "
                                   NGX_CONF_PATH ")" NGX_LINEFEED
                "  -g directives : set global directives out of configuration "
                                   "file" NGX_LINEFEED NGX_LINEFEED
                );
        }

        // 打印配置信息(包含的模块等)
        if (ngx_show_configure) {

#ifdef NGX_COMPILER
            ngx_write_stderr("built by " NGX_COMPILER NGX_LINEFEED);
#endif

#if (NGX_SSL)
            if (SSLeay() == SSLEAY_VERSION_NUMBER) {
                ngx_write_stderr("built with " OPENSSL_VERSION_TEXT
                                 NGX_LINEFEED);
            } else {
                ngx_write_stderr("built with " OPENSSL_VERSION_TEXT
                                 " (running with ");
                ngx_write_stderr((char *) (uintptr_t)
                                 SSLeay_version(SSLEAY_VERSION));
                ngx_write_stderr(")" NGX_LINEFEED);
            }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
            ngx_write_stderr("TLS SNI support enabled" NGX_LINEFEED);
#else
            ngx_write_stderr("TLS SNI support disabled" NGX_LINEFEED);
#endif
#endif

            ngx_write_stderr("configure arguments:" NGX_CONFIGURE NGX_LINEFEED);
        }

        if (!ngx_test_config) {
            return 0;
        }
    }

    /* TODO */ ngx_max_sockets = -1;

    //初始化nginx运行过程中用的的时间,在运行过程中如果需要时间的话,都是直接从这里的缓存中获取的
    ngx_time_init();

#if (NGX_PCRE)
    ngx_regex_init();
#endif

    // 获取进程id
    ngx_pid = ngx_getpid();

    // 如果ngx_prefix没有值,这一步会为其设置
    log = ngx_log_init(ngx_prefix);
    if (log == NULL) {
        return 1;
    }

    /* STUB */
#if (NGX_OPENSSL)
    ngx_ssl_init(log);
#endif

    /*
     * init_cycle->log is required for signal handlers and
     * ngx_process_options()
     */

    // 清零init_cycle对象占用的内存,防止有脏数据
    ngx_memzero(&init_cycle, sizeof(ngx_cycle_t));
    init_cycle.log = log;
    // 实际上init_cycle是在栈上分配的内存,但是这个栈是main方法所在的栈,只是一个临时使用
    ngx_cycle = &init_cycle;

    init_cycle.pool = ngx_create_pool(1024, log);
    if (init_cycle.pool == NULL) {
        return 1;
    }

    // 保存启动nginx时候的入参值(保存到ngx_argc、ngx_argv、ngx_os_argv中)
    if (ngx_save_argv(&init_cycle, argc, argv) != NGX_OK) {
        return 1;
    }

    /*
     * 主要用来设置文件路径和前缀等,如cycle->conf_prefix、conf_file等
     * 这个方法处理完之后cycle->conf_file就变成了完整的路径
     */
    if (ngx_process_options(&init_cycle) != NGX_OK) {
        return 1;
    }

    // 初始化一些跟操作系统相关的变量
    if (ngx_os_init(log) != NGX_OK) {
        return 1;
    }

    /*
     * ngx_crc32_table_init() requires ngx_cacheline_size set in ngx_os_init()
     */
    if (ngx_crc32_table_init() != NGX_OK) {
        return 1;
    }

    /*
     * 初始化init_cycle->listening,如果环境变量getenv(NGINX_VAR)存在的话
     * 数组init_cycle->listening用来存放ngx_listening_t对象
     * ngx_listening_t结构体存放了nginx用来监听的ip和端口号
     */
    if (ngx_add_inherited_sockets(&init_cycle) != NGX_OK) {
        return 1;
    }

    // 为nginx的所有模块加编号,不区分核心模块和非核心模块
    ngx_max_module = 0;
    for (i = 0; ngx_modules[i]; i++) {
        ngx_modules[i]->index = ngx_max_module++;
    }

    /*
     * 走到这里才开始去读配置文件
     *
     * 1.初始化各个NGX_CORE_MODULE模块的init_cycle->conf_ctx
     * 2.调用NGX_CONF_MODLUE模块的ngx_conf_parse方法去解析配置文件
     *
     * init_cycle只用来传递一些配置文件路径信息,临时使用,返回的cycle才是真正的后续要用到的
     *
     * 该方法是各个模块创建和初始化自己结构的入口
     */
    cycle = ngx_init_cycle(&init_cycle);
    if (cycle == NULL) {
        if (ngx_test_config) {
            ngx_log_stderr(0, "configuration file %s test failed",
                           init_cycle.conf_file.data);
        }

        return 1;
    }

    if (ngx_test_config) {
        if (!ngx_quiet_mode) {
            ngx_log_stderr(0, "configuration file %s test is successful",
                           cycle->conf_file.data);
        }

        if (ngx_dump_config) {
            cd = cycle->config_dump.elts;

            for (i = 0; i < cycle->config_dump.nelts; i++) {

                ngx_write_stdout("# configuration file ");
                (void) ngx_write_fd(ngx_stdout, cd[i].name.data,
                                    cd[i].name.len);
                ngx_write_stdout(":" NGX_LINEFEED);

                b = cd[i].buffer;

                (void) ngx_write_fd(ngx_stdout, b->pos, b->last - b->pos);
                ngx_write_stdout(NGX_LINEFEED);
            }
        }

        return 0;
    }

    if (ngx_signal) {
    	/*
    	 * 处理 stop、quit、reopen、reload这四个信号,这四个信号要求当前nginx是已启动状态的,
    	 * 因为下面的方法用到了nginx的主进程id(在文件nginx.pid中)。
    	 *
    	 * 信号函数已经有/src/os/unix/ngx_process.c/ngx_init_signals方法负责绑定。
    	 *
    	 * 发送完信号之后该进程就退出了,前面一系列操作的意义就是验证配置的正确性,如果不正确就不应该发信息号。
    	 *
    	 * 信号有主进程的/src/os/unix/ngx_process_cycle.c/ngx_master_process_cycle方法接收处理(master-worker模式)
    	 */
        return ngx_signal_process(cycle, ngx_signal);
    }


    ngx_os_status(cycle->log);

    ngx_cycle = cycle;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    /*
     * TODO 为啥在这里比较ngx_process
     * 疑问:在什么情况下ccf->master == 1 并且 ngx_process != NGX_PROCESS_SINGLE
     */
    if (ccf->master && ngx_process == NGX_PROCESS_SINGLE) {
        ngx_process = NGX_PROCESS_MASTER;
    }

#if !(NGX_WIN32)

    /*
     * 为master进程绑定signals[]数组中定义的信号
     * 其中有四种信号,对应了ngx中的四个参数命令
     * 	reload:SIGHUP
     * 	reopen:SIGUSR1
     * 	stop:SIGTERM
     *	quit:SIGQUIT
     *
     * 调用系统函数sigaction
     */
    if (ngx_init_signals(cycle->log) != NGX_OK) {
        return 1;
    }

    /*
     * 如果nginx要以守护进程的形式启动,那么ngx_daemon方法会fork一个子进程,主进程则直接返回。
     *
     */
    if (!ngx_inherited && ccf->daemon) {
        if (ngx_daemon(cycle->log) != NGX_OK) {
            return 1;
        }

        // deamon成功则标记当前进程已经是守护进程了
        ngx_daemonized = 1;
    }

    if (ngx_inherited) {
        ngx_daemonized = 1;
    }

#endif

    // 创建用于存放主进程pid的文件,并把ngx_pid放入该文件
    if (ngx_create_pidfile(&ccf->pid, cycle->log) != NGX_OK) {
        return 1;
    }

    // 将标准错误输出定位到log指定的文件描述符中
    if (ngx_log_redirect_stderr(cycle) != NGX_OK) {
        return 1;
    }

    if (log->file->fd != ngx_stderr) {
    	/*
    	 * 因为已经把标准错误文件描述符定位到fd代表的error.log文件了,所以这个fd就没有了
    	 * 想输出错误信息到error.log文件,则直接用标准错误文件描述符就可以了
    	 */
        if (ngx_close_file(log->file->fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          ngx_close_file_n " built-in log failed");
        }
    }

    ngx_use_stderr = 0;

    // 这里为啥不用ccf->master标记来判断 TODO
    if (ngx_process == NGX_PROCESS_SINGLE) {
    	// 单进程模式
        ngx_single_process_cycle(cycle);

    } else {
    	// master-worker模式
        ngx_master_process_cycle(cycle);
    }

    return 0;
}


/*
 * 继承环境变量NGINX_VAR中的一些数据
 *
 * 用它初始化cycle->listening
 *
 * TODO 细节 平滑升级? 二进制升级?
 */
static ngx_int_t
ngx_add_inherited_sockets(ngx_cycle_t *cycle)
{
    u_char           *p, *v, *inherited;
    ngx_int_t         s;
    ngx_listening_t  *ls;

    // 获取操作系统中环境变量NGINX_VAR的值
    inherited = (u_char *) getenv(NGINX_VAR);

    if (inherited == NULL) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "using inherited sockets from \"%s\"", inherited);

    if (ngx_array_init(&cycle->listening, cycle->pool, 10,
                       sizeof(ngx_listening_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    for (p = inherited, v = p; *p; p++) {
        if (*p == ':' || *p == ';') {
            s = ngx_atoi(v, p - v);
            if (s == NGX_ERROR) {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                              "invalid socket number \"%s\" in " NGINX_VAR
                              " environment variable, ignoring the rest"
                              " of the variable", v);
                break;
            }

            v = p + 1;

            ls = ngx_array_push(&cycle->listening);
            if (ls == NULL) {
                return NGX_ERROR;
            }

            ngx_memzero(ls, sizeof(ngx_listening_t));

            ls->fd = (ngx_socket_t) s;
        }
    }

    ngx_inherited = 1;

    return ngx_set_inherited_sockets(cycle);
}


char **
ngx_set_environment(ngx_cycle_t *cycle, ngx_uint_t *last)
{
    char             **p, **env;
    ngx_str_t         *var;
    ngx_uint_t         i, n;
    ngx_core_conf_t   *ccf;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    if (last == NULL && ccf->environment) {
        return ccf->environment;
    }

    var = ccf->env.elts;

    for (i = 0; i < ccf->env.nelts; i++) {
        if (ngx_strcmp(var[i].data, "TZ") == 0
            || ngx_strncmp(var[i].data, "TZ=", 3) == 0)
        {
            goto tz_found;
        }
    }

    var = ngx_array_push(&ccf->env);
    if (var == NULL) {
        return NULL;
    }

    var->len = 2;
    var->data = (u_char *) "TZ";

    var = ccf->env.elts;

tz_found:

    n = 0;

    for (i = 0; i < ccf->env.nelts; i++) {

        if (var[i].data[var[i].len] == '=') {
            n++;
            continue;
        }

        for (p = ngx_os_environ; *p; p++) {

            if (ngx_strncmp(*p, var[i].data, var[i].len) == 0
                && (*p)[var[i].len] == '=')
            {
                n++;
                break;
            }
        }
    }

    if (last) {
        env = ngx_alloc((*last + n + 1) * sizeof(char *), cycle->log);
        *last = n;

    } else {
        env = ngx_palloc(cycle->pool, (n + 1) * sizeof(char *));
    }

    if (env == NULL) {
        return NULL;
    }

    n = 0;

    for (i = 0; i < ccf->env.nelts; i++) {

        if (var[i].data[var[i].len] == '=') {
            env[n++] = (char *) var[i].data;
            continue;
        }

        for (p = ngx_os_environ; *p; p++) {

            if (ngx_strncmp(*p, var[i].data, var[i].len) == 0
                && (*p)[var[i].len] == '=')
            {
                env[n++] = *p;
                break;
            }
        }
    }

    env[n] = NULL;

    if (last == NULL) {
        ccf->environment = env;
        environ = env;
    }

    return env;
}


/*
 * 平滑升级用 TODO
 */
ngx_pid_t
ngx_exec_new_binary(ngx_cycle_t *cycle, char *const *argv)
{
    char             **env, *var;
    u_char            *p;
    ngx_uint_t         i, n;
    ngx_pid_t          pid;
    ngx_exec_ctx_t     ctx;
    ngx_core_conf_t   *ccf;
    ngx_listening_t   *ls;

    ngx_memzero(&ctx, sizeof(ngx_exec_ctx_t));

    ctx.path = argv[0];
    ctx.name = "new binary process";
    ctx.argv = argv;

    n = 2;
    env = ngx_set_environment(cycle, &n);
    if (env == NULL) {
        return NGX_INVALID_PID;
    }

    var = ngx_alloc(sizeof(NGINX_VAR)
                    + cycle->listening.nelts * (NGX_INT32_LEN + 1) + 2,
                    cycle->log);
    if (var == NULL) {
        ngx_free(env);
        return NGX_INVALID_PID;
    }

    p = ngx_cpymem(var, NGINX_VAR "=", sizeof(NGINX_VAR));

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {
        p = ngx_sprintf(p, "%ud;", ls[i].fd);
    }

    *p = '\0';

    // 这边设置了一个环境变量
    env[n++] = var;

#if (NGX_SETPROCTITLE_USES_ENV)

    /* allocate the spare 300 bytes for the new binary process title */

    env[n++] = "SPARE=XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
               "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
               "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
               "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
               "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";

#endif

    env[n] = NULL;

#if (NGX_DEBUG)
    {
    char  **e;
    for (e = env; *e; e++) {
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0, "env: %s", *e);
    }
    }
#endif

    ctx.envp = (char *const *) env;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    if (ngx_rename_file(ccf->pid.data, ccf->oldpid.data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      ngx_rename_file_n " %s to %s failed "
                      "before executing new binary process \"%s\"",
                      ccf->pid.data, ccf->oldpid.data, argv[0]);

        ngx_free(env);
        ngx_free(var);

        return NGX_INVALID_PID;
    }

    pid = ngx_execute(cycle, &ctx);

    if (pid == NGX_INVALID_PID) {
        if (ngx_rename_file(ccf->oldpid.data, ccf->pid.data)
            == NGX_FILE_ERROR)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          ngx_rename_file_n " %s back to %s failed after "
                          "an attempt to execute new binary process \"%s\"",
                          ccf->oldpid.data, ccf->pid.data, argv[0]);
        }
    }

    ngx_free(env);
    ngx_free(var);

    return pid;
}


/**
 * 解析启动nginx时指定的入参
 *
 */
static ngx_int_t
ngx_get_options(int argc, char *const *argv)
{
    u_char     *p;
    ngx_int_t   i;

    for (i = 1; i < argc; i++) {

    	// 第i个入参字符串(如 -V、-s等)
        p = (u_char *) argv[i];

        // 如果入参不是以字符 "-" 开始的则报错; 说明nginx入参必须以"-"开始
        if (*p++ != '-') {
            ngx_log_stderr(0, "invalid option: \"%s\"", argv[i]);
            return NGX_ERROR;
        }

        while (*p) {

            switch (*p++) {

            // 入参以 -? 或 -h 开始说明是要看帮助,则打印版本号和帮助文档
            case '?':
            case 'h':
                ngx_show_version = 1;
                ngx_show_help = 1;
                break;

            // 入参以 -v 开始说明是要看版本号
            case 'v':
                ngx_show_version = 1;
                break;

            // 入参以 -V 开始,打印版本号和配置信息(如,包括什么模块、被什么编译器编译的等信息)
            case 'V':
                ngx_show_version = 1;
                ngx_show_configure = 1;
                break;

            // 入参以 -t 开始,测试配置文件是否ok
            case 't':
                ngx_test_config = 1;
                break;

            // 入参以 -T 开始, //TODO
            case 'T':
                ngx_test_config = 1;
                ngx_dump_config = 1;
                break;

            case 'q':
                ngx_quiet_mode = 1;
                break;

            case 'p':
                if (*p) {
                    ngx_prefix = p;
                    goto next;
                }

                if (argv[++i]) {
                    ngx_prefix = (u_char *) argv[i];
                    goto next;
                }

                ngx_log_stderr(0, "option \"-p\" requires directory name");
                return NGX_ERROR;

            // 以 -c 开始  指定配置文件
            case 'c':
                if (*p) {
                    ngx_conf_file = p;
                    goto next;
                }

                if (argv[++i]) {
                    ngx_conf_file = (u_char *) argv[i];
                    goto next;
                }

                ngx_log_stderr(0, "option \"-c\" requires file name");
                return NGX_ERROR;

            case 'g':
                if (*p) {
                    ngx_conf_params = p;
                    goto next;
                }

                if (argv[++i]) {
                    ngx_conf_params = (u_char *) argv[i];
                    goto next;
                }

                ngx_log_stderr(0, "option \"-g\" requires parameter");
                return NGX_ERROR;

            // 入参以 -s(信号) 开始
            case 's':
                if (*p) {
                	// -sreload(命令未加空格)
                    ngx_signal = (char *) p;

                } else if (argv[++i]) {
                	// -s reload(命令加空格了)
                    ngx_signal = argv[i];

                } else {
                	// -s  (命令后什么也没输入)
                    ngx_log_stderr(0, "option \"-s\" requires parameter");
                    return NGX_ERROR;
                }

                if (ngx_strcmp(ngx_signal, "stop") == 0
                    || ngx_strcmp(ngx_signal, "quit") == 0
                    || ngx_strcmp(ngx_signal, "reopen") == 0
                    || ngx_strcmp(ngx_signal, "reload") == 0)
                {
                    ngx_process = NGX_PROCESS_SIGNALLER;
                    goto next;
                }

                // 如果不是stop、quit、reopen、reload这四中信号则返回错误
                ngx_log_stderr(0, "invalid option: \"-s %s\"", ngx_signal);
                return NGX_ERROR;

            default:
                ngx_log_stderr(0, "invalid option: \"%c\"", *(p - 1));
                return NGX_ERROR;
            }
        }

    next:

        continue;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_save_argv(ngx_cycle_t *cycle, int argc, char *const *argv)
{
#if (NGX_FREEBSD)

    ngx_os_argv = (char **) argv;
    ngx_argc = argc;
    ngx_argv = (char **) argv;

#else
    size_t     len;
    ngx_int_t  i;

    ngx_os_argv = (char **) argv;
    ngx_argc = argc;

    ngx_argv = ngx_alloc((argc + 1) * sizeof(char *), cycle->log);
    if (ngx_argv == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < argc; i++) {
        len = ngx_strlen(argv[i]) + 1;

        ngx_argv[i] = ngx_alloc(len, cycle->log);
        if (ngx_argv[i] == NULL) {
            return NGX_ERROR;
        }

        (void) ngx_cpystrn((u_char *) ngx_argv[i], (u_char *) argv[i], len);
    }

    // 入参argv最后一个值是NULL
    ngx_argv[i] = NULL;

#endif

    // 环境变量最后一个值也是NULL
    ngx_os_environ = environ;

    return NGX_OK;
}


/**
 * 主要用来设置文件路劲和安装路径等
 * 如cycle->conf_prefix、conf_file等
 *
 */
static ngx_int_t
ngx_process_options(ngx_cycle_t *cycle)
{
    u_char  *p;
    size_t   len;

    // 整个if语句用来设置对象cycle中的 prefix(安装) 和 conf_prefix(配置文件路径)
    if (ngx_prefix) {
        len = ngx_strlen(ngx_prefix);
        p = ngx_prefix;

        if (len && !ngx_path_separator(p[len - 1])) {
            p = ngx_pnalloc(cycle->pool, len + 1);
            if (p == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(p, ngx_prefix, len);
            p[len++] = '/';
        }

        cycle->conf_prefix.len = len;
        cycle->conf_prefix.data = p;
        cycle->prefix.len = len;
        cycle->prefix.data = p;

    } else {

#ifndef NGX_PREFIX

        p = ngx_pnalloc(cycle->pool, NGX_MAX_PATH);
        if (p == NULL) {
            return NGX_ERROR;
        }

        if (ngx_getcwd(p, NGX_MAX_PATH) == 0) {
            ngx_log_stderr(ngx_errno, "[emerg]: " ngx_getcwd_n " failed");
            return NGX_ERROR;
        }

        len = ngx_strlen(p);

        p[len++] = '/';

        cycle->conf_prefix.len = len;
        cycle->conf_prefix.data = p;
        cycle->prefix.len = len;
        cycle->prefix.data = p;

#else

#ifdef NGX_CONF_PREFIX
        ngx_str_set(&cycle->conf_prefix, NGX_CONF_PREFIX);
#else
        ngx_str_set(&cycle->conf_prefix, NGX_PREFIX);
#endif
        ngx_str_set(&cycle->prefix, NGX_PREFIX);

#endif
    }

    // 向对象cycle中的配置文件路径赋值,相对路径
    if (ngx_conf_file) {
    	// 如果用户启动nginx时使用了-c来指定文件路径,则使用给用户指定的文件路径
        cycle->conf_file.len = ngx_strlen(ngx_conf_file);
        cycle->conf_file.data = ngx_conf_file;

    } else {
    	// 启动nginx时用户未使用-c指定配置文件路径,则使用默认文件路径
        ngx_str_set(&cycle->conf_file, NGX_CONF_PATH);
    }

    // 截止到这里 cycle->conf_prefix和cycle->prefix的值还是一样的
    // 到这里cycle->conf_file只是一个相对路径,下面这个方法是要把他变成绝对路径
    if (ngx_conf_full_name(cycle, &cycle->conf_file, 0) != NGX_OK) {
        return NGX_ERROR;
    }

    for (p = cycle->conf_file.data + cycle->conf_file.len - 1;
         p > cycle->conf_file.data;
         p--)
    {
    	// 从这里开始cycle->conf_prefix和cycle->prefix的值就不一定一样了
    	// 从cycle->conf_file里面解析出配置文件前缀
    	// 比如cycle->conf_file = "/my/path/nginx.conf",执行完下面的代码后
    	// cycle->conf_prefix就变成了"/my/path/"
        if (ngx_path_separator(*p)) {
            cycle->conf_prefix.len = p - ngx_cycle->conf_file.data + 1;
            cycle->conf_prefix.data = ngx_cycle->conf_file.data;
            break;
        }
    }

    // ngx_conf_params保存的是-g选项指定的指令
    if (ngx_conf_params) {
        cycle->conf_param.len = ngx_strlen(ngx_conf_params);
        cycle->conf_param.data = ngx_conf_params;
    }

    if (ngx_test_config) {
        cycle->log->log_level = NGX_LOG_INFO;
    }

    return NGX_OK;
}


static void *
ngx_core_module_create_conf(ngx_cycle_t *cycle)
{
    ngx_core_conf_t  *ccf;

    ccf = ngx_pcalloc(cycle->pool, sizeof(ngx_core_conf_t));
    if (ccf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc()
     *
     *     ccf->pid = NULL;
     *     ccf->oldpid = NULL;
     *     ccf->priority = 0;
     *     ccf->cpu_affinity_n = 0;
     *     ccf->cpu_affinity = NULL;
     */

    ccf->daemon = NGX_CONF_UNSET;
    ccf->master = NGX_CONF_UNSET;
    ccf->timer_resolution = NGX_CONF_UNSET_MSEC;

    ccf->worker_processes = NGX_CONF_UNSET;
    ccf->debug_points = NGX_CONF_UNSET;

    ccf->rlimit_nofile = NGX_CONF_UNSET;
    ccf->rlimit_core = NGX_CONF_UNSET;

    ccf->user = (ngx_uid_t) NGX_CONF_UNSET_UINT;
    ccf->group = (ngx_gid_t) NGX_CONF_UNSET_UINT;

    if (ngx_array_init(&ccf->env, cycle->pool, 1, sizeof(ngx_str_t))
        != NGX_OK)
    {
        return NULL;
    }

    return ccf;
}


static char *
ngx_core_module_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_core_conf_t  *ccf = conf;

    ngx_conf_init_value(ccf->daemon, 1);
    ngx_conf_init_value(ccf->master, 1);
    ngx_conf_init_msec_value(ccf->timer_resolution, 0);

    ngx_conf_init_value(ccf->worker_processes, 1);
    ngx_conf_init_value(ccf->debug_points, 0);

#if (NGX_HAVE_CPU_AFFINITY)

    if (ccf->cpu_affinity_n
        && ccf->cpu_affinity_n != 1
        && ccf->cpu_affinity_n != (ngx_uint_t) ccf->worker_processes)
    {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "the number of \"worker_processes\" is not equal to "
                      "the number of \"worker_cpu_affinity\" masks, "
                      "using last mask for remaining worker processes");
    }

#endif


    if (ccf->pid.len == 0) {
        ngx_str_set(&ccf->pid, NGX_PID_PATH);
    }

    if (ngx_conf_full_name(cycle, &ccf->pid, 0) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    ccf->oldpid.len = ccf->pid.len + sizeof(NGX_OLDPID_EXT);

    ccf->oldpid.data = ngx_pnalloc(cycle->pool, ccf->oldpid.len);
    if (ccf->oldpid.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(ngx_cpymem(ccf->oldpid.data, ccf->pid.data, ccf->pid.len),
               NGX_OLDPID_EXT, sizeof(NGX_OLDPID_EXT));


#if !(NGX_WIN32)

    if (ccf->user == (uid_t) NGX_CONF_UNSET_UINT && geteuid() == 0) {
        struct group   *grp;
        struct passwd  *pwd;

        ngx_set_errno(0);
        pwd = getpwnam(NGX_USER);
        if (pwd == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "getpwnam(\"" NGX_USER "\") failed");
            return NGX_CONF_ERROR;
        }

        ccf->username = NGX_USER;
        ccf->user = pwd->pw_uid;

        ngx_set_errno(0);
        grp = getgrnam(NGX_GROUP);
        if (grp == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "getgrnam(\"" NGX_GROUP "\") failed");
            return NGX_CONF_ERROR;
        }

        ccf->group = grp->gr_gid;
    }


    if (ccf->lock_file.len == 0) {
        ngx_str_set(&ccf->lock_file, NGX_LOCK_PATH);
    }

    if (ngx_conf_full_name(cycle, &ccf->lock_file, 0) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    {
    ngx_str_t  lock_file;

    lock_file = cycle->old_cycle->lock_file;

    if (lock_file.len) {
        lock_file.len--;

        if (ccf->lock_file.len != lock_file.len
            || ngx_strncmp(ccf->lock_file.data, lock_file.data, lock_file.len)
               != 0)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "\"lock_file\" could not be changed, ignored");
        }

        cycle->lock_file.len = lock_file.len + 1;
        lock_file.len += sizeof(".accept");

        cycle->lock_file.data = ngx_pstrdup(cycle->pool, &lock_file);
        if (cycle->lock_file.data == NULL) {
            return NGX_CONF_ERROR;
        }

    } else {
        cycle->lock_file.len = ccf->lock_file.len + 1;
        cycle->lock_file.data = ngx_pnalloc(cycle->pool,
                                      ccf->lock_file.len + sizeof(".accept"));
        if (cycle->lock_file.data == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_memcpy(ngx_cpymem(cycle->lock_file.data, ccf->lock_file.data,
                              ccf->lock_file.len),
                   ".accept", sizeof(".accept"));
    }
    }

#endif

    return NGX_CONF_OK;
}


static char *
ngx_set_user(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
#if (NGX_WIN32)

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "\"user\" is not supported, ignored");

    return NGX_CONF_OK;

#else

    ngx_core_conf_t  *ccf = conf;

    char             *group;
    struct passwd    *pwd;
    struct group     *grp;
    ngx_str_t        *value;

    if (ccf->user != (uid_t) NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    if (geteuid() != 0) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "the \"user\" directive makes sense only "
                           "if the master process runs "
                           "with super-user privileges, ignored");
        return NGX_CONF_OK;
    }

    value = (ngx_str_t *) cf->args->elts;

    ccf->username = (char *) value[1].data;

    ngx_set_errno(0);
    pwd = getpwnam((const char *) value[1].data);
    if (pwd == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "getpwnam(\"%s\") failed", value[1].data);
        return NGX_CONF_ERROR;
    }

    ccf->user = pwd->pw_uid;

    group = (char *) ((cf->args->nelts == 2) ? value[1].data : value[2].data);

    ngx_set_errno(0);
    grp = getgrnam(group);
    if (grp == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "getgrnam(\"%s\") failed", group);
        return NGX_CONF_ERROR;
    }

    ccf->group = grp->gr_gid;

    return NGX_CONF_OK;

#endif
}


static char *
ngx_set_env(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_core_conf_t  *ccf = conf;

    ngx_str_t   *value, *var;
    ngx_uint_t   i;

    var = ngx_array_push(&ccf->env);
    if (var == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;
    *var = value[1];

    for (i = 0; i < value[1].len; i++) {

        if (value[1].data[i] == '=') {

            var->len = i;

            return NGX_CONF_OK;
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_set_priority(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_core_conf_t  *ccf = conf;

    ngx_str_t        *value;
    ngx_uint_t        n, minus;

    if (ccf->priority != 0) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (value[1].data[0] == '-') {
        n = 1;
        minus = 1;

    } else if (value[1].data[0] == '+') {
        n = 1;
        minus = 0;

    } else {
        n = 0;
        minus = 0;
    }

    ccf->priority = ngx_atoi(&value[1].data[n], value[1].len - n);
    if (ccf->priority == NGX_ERROR) {
        return "invalid number";
    }

    if (minus) {
        ccf->priority = -ccf->priority;
    }

    return NGX_CONF_OK;
}


static char *
ngx_set_cpu_affinity(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
#if (NGX_HAVE_CPU_AFFINITY)
    ngx_core_conf_t  *ccf = conf;

    u_char            ch;
    uint64_t         *mask;
    ngx_str_t        *value;
    ngx_uint_t        i, n;

    if (ccf->cpu_affinity) {
        return "is duplicate";
    }

    mask = ngx_palloc(cf->pool, (cf->args->nelts - 1) * sizeof(uint64_t));
    if (mask == NULL) {
        return NGX_CONF_ERROR;
    }

    ccf->cpu_affinity_n = cf->args->nelts - 1;
    ccf->cpu_affinity = mask;

    value = cf->args->elts;

    for (n = 1; n < cf->args->nelts; n++) {

        if (value[n].len > 64) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                         "\"worker_cpu_affinity\" supports up to 64 CPUs only");
            return NGX_CONF_ERROR;
        }

        mask[n - 1] = 0;

        for (i = 0; i < value[n].len; i++) {

            ch = value[n].data[i];

            if (ch == ' ') {
                continue;
            }

            mask[n - 1] <<= 1;

            if (ch == '0') {
                continue;
            }

            if (ch == '1') {
                mask[n - 1] |= 1;
                continue;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "invalid character \"%c\" in \"worker_cpu_affinity\"",
                          ch);
            return NGX_CONF_ERROR;
        }
    }

#else

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "\"worker_cpu_affinity\" is not supported "
                       "on this platform, ignored");
#endif

    return NGX_CONF_OK;
}


uint64_t
ngx_get_cpu_affinity(ngx_uint_t n)
{
    ngx_core_conf_t  *ccf;

    ccf = (ngx_core_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                           ngx_core_module);

    if (ccf->cpu_affinity == NULL) {
        return 0;
    }

    if (ccf->cpu_affinity_n > n) {
        return ccf->cpu_affinity[n];
    }

    return ccf->cpu_affinity[ccf->cpu_affinity_n - 1];
}


static char *
ngx_set_worker_processes(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t        *value;
    ngx_core_conf_t  *ccf;

    ccf = (ngx_core_conf_t *) conf;

    if (ccf->worker_processes != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = (ngx_str_t *) cf->args->elts;

    if (ngx_strcmp(value[1].data, "auto") == 0) {
        ccf->worker_processes = ngx_ncpu;
        return NGX_CONF_OK;
    }

    ccf->worker_processes = ngx_atoi(value[1].data, value[1].len);

    if (ccf->worker_processes == NGX_ERROR) {
        return "invalid value";
    }

    return NGX_CONF_OK;
}
