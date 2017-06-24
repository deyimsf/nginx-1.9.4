
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


#if (NGX_SETPROCTITLE_USES_ENV)

/*
 * To change the process title in Linux and Solaris we have to set argv[1]
 * to NULL and to copy the title to the same place where the argv[0] points to.
 * However, argv[0] may be too small to hold a new title.  Fortunately, Linux
 * and Solaris store argv[] and environ[] one after another.  So we should
 * ensure that is the continuous memory and then we allocate the new memory
 * for environ[] and copy it.  After this we could use the memory starting
 * from argv[0] for our process title.
 *
 * The Solaris's standard /bin/ps does not show the changed process title.
 * You have to use "/usr/ucb/ps -w" instead.  Besides, the UCB ps does not
 * show a new title if its length less than the origin command line length.
 * To avoid it we append to a new title the origin command line in the
 * parenthesis.
 */

extern char **environ;

static char *ngx_os_argv_last;

/*
 * 为修改进程名字做一些初始化工作
 *
 * 1.拷贝一份environ变量,并将environ中的指针指向新拷贝的内容(如果argv和environ内存连续存放)
 * 2.ngx_os_argv_last指向argv或者environ的最后一个有效字符的后面(也就是不包含最后的NULL)
 */
ngx_int_t
ngx_init_setproctitle(ngx_log_t *log)
{
    u_char      *p;
    size_t       size;
    ngx_uint_t   i;

    size = 0;

    // 计算当前进程存放环境变量需要的内存大小
    for (i = 0; environ[i]; i++) {
        size += ngx_strlen(environ[i]) + 1;
    }

    // 创建的内存用来存储环境变量
    p = ngx_alloc(size, log);
    if (p == NULL) {
        return NGX_ERROR;
    }

    /*
     * ngx_os_argv_last最终会指向argv或者environ的最后一个有效字符的后面
     */
    ngx_os_argv_last = ngx_os_argv[0];

    for (i = 0; ngx_os_argv[i]; i++) {
        if (ngx_os_argv_last == ngx_os_argv[i]) {
            ngx_os_argv_last = ngx_os_argv[i] + ngx_strlen(ngx_os_argv[i]) + 1;
        }
    }

    /*
     * 如果可以完全确认argv和environ内存是连续的,则上面的循环就是多余的。
     *
     * 如果argv和environ不是连续存放的,则下面这个for循环中的if语句可以确保,
     * ngx_os_argv_last不会指向非法的地址。
     */
    for (i = 0; environ[i]; i++) {

    	// 如果不相等则说明argv和environ没有存放在连续的空间中
        if (ngx_os_argv_last == environ[i]) {

            size = ngx_strlen(environ[i]) + 1;
            ngx_os_argv_last = environ[i] + size;

            ngx_cpystrn(p, (u_char *) environ[i], size);
            environ[i] = (char *) p;
            p += size;
        }
    }

    /*
     * 指向argv或者environ的最后一个有效字符的后面,这里 -- 是为了去掉NULL
     * 因为不管是argv还是environ最后都是NULL
     */
    ngx_os_argv_last--;

    return NGX_OK;
}


/*
 * 设置当前进程的名字
 */
void
ngx_setproctitle(char *title)
{
    u_char     *p;

#if (NGX_SOLARIS)

    ngx_int_t   i;
    size_t      size;

#endif

    // TODO 感觉没必要呢? 什么特殊操作系统需要将argv[1]设置为NULL
    ngx_os_argv[1] = NULL;

    /*
     * 为进程名字加上nginx: 前缀
     * 在该方法中,如果在第二参数中遇到'\0'则结束拷贝
     */
    p = ngx_cpystrn((u_char *) ngx_os_argv[0], (u_char *) "nginx: ",
                    ngx_os_argv_last - ngx_os_argv[0]);

    p = ngx_cpystrn(p, (u_char *) title, ngx_os_argv_last - (char *) p);

#if (NGX_SOLARIS)

    size = 0;

    for (i = 0; i < ngx_argc; i++) {
        size += ngx_strlen(ngx_argv[i]) + 1;
    }

    if (size > (size_t) ((char *) p - ngx_os_argv[0])) {

        /*
         * ngx_setproctitle() is too rare operation so we use
         * the non-optimized copies
         */

        p = ngx_cpystrn(p, (u_char *) " (", ngx_os_argv_last - (char *) p);

        for (i = 0; i < ngx_argc; i++) {
            p = ngx_cpystrn(p, (u_char *) ngx_argv[i],
                            ngx_os_argv_last - (char *) p);
            p = ngx_cpystrn(p, (u_char *) " ", ngx_os_argv_last - (char *) p);
        }

        if (*(p - 1) == ' ') {
            *(p - 1) = ')';
        }
    }

#endif

    // 剩下的内存清空(都设置为'\0')
    if (ngx_os_argv_last - (char *) p) {
        ngx_memset(p, NGX_SETPROCTITLE_PAD, ngx_os_argv_last - (char *) p);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                   "setproctitle: \"%s\"", ngx_os_argv[0]);
}

#endif /* NGX_SETPROCTITLE_USES_ENV */
