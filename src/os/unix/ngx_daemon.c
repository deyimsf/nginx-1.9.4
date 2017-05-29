
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


ngx_int_t
ngx_daemon(ngx_log_t *log)
{
    int  fd;

    switch (fork()) {
    case -1:
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "fork() failed");
        return NGX_ERROR;

    case 0:
    	// 子进程
        break;

    default:
    	// 主进程直接退出
        exit(0);
    }

    /* 以下代码有子进程执行 */

    ngx_pid = ngx_getpid();

    /*
     * 每打开一个终端或一个登入都是一个会话,每个会话都有一个唯一的前台进程组和多个后台进程组,
     * 只有前台进程组才会有会话终端。
     *
     * 子进程调用setsid方法会使子进程晋升为进程组长,如果进程已经是进程组长则返回错误。
     * 这样子进程就和原来的进程完全脱离了关系,并且子进程也不会被分配控制终端。
     *
     * 如果想重新打开控制终端可以调用open("/dev/ttyn")方法。
     * 如果不想该进程重新打开控制终端,则可以在fork()一次,这样第一个子进程退出,新fork()出的子进程
     * 就变成了当前进程,并且因为新fork出的进程不是进程组长,所以就无法重新再打开控制终端。
     */
    if (setsid() == -1) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "setsid() failed");
        return NGX_ERROR;
    }

    umask(0);

    fd = open("/dev/null", O_RDWR);
    if (fd == -1) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "open(\"/dev/null\") failed");
        return NGX_ERROR;
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "dup2(STDIN) failed");
        return NGX_ERROR;
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "dup2(STDOUT) failed");
        return NGX_ERROR;
    }

#if 0
    if (dup2(fd, STDERR_FILENO) == -1) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "dup2(STDERR) failed");
        return NGX_ERROR;
    }
#endif

    if (fd > STDERR_FILENO) {
        if (close(fd) == -1) {
            ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "close() failed");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}
