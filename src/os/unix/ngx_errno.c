
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


/*
 * The strerror() messages are copied because:
 *
 * 1) strerror() and strerror_r() functions are not Async-Signal-Safe,
 *    therefore, they cannot be used in signal handlers;
 *
 *    strerror() and strerror_r()这两个函数不是"异步信号安全"的。
 *    异步信号安全函数是指,在函数内部即使因为信号正在被中断,在其它地方该函数被调用了也不会有问题。
 *    如果在函数中存在更新静态区域里的数据情况(如，malloc),就不是异步信号安全
 *
 * 2) a direct sys_errlist[] array may be used instead of these functions,
 *    but Linux linker warns about its usage:
 *
 * warning: `sys_errlist' is deprecated; use `strerror' or `strerror_r' instead
 * warning: `sys_nerr' is deprecated; use `strerror' or `strerror_r' instead
 *
 *    causing false bug reports.
 */


static ngx_str_t  *ngx_sys_errlist;
static ngx_str_t   ngx_unknown_error = ngx_string("Unknown error");


u_char *
ngx_strerror(ngx_err_t err, u_char *errstr, size_t size)
{
    ngx_str_t  *msg;

    msg = ((ngx_uint_t) err < NGX_SYS_NERR) ? &ngx_sys_errlist[err]:
                                              &ngx_unknown_error;
    size = ngx_min(size, msg->len);

    return ngx_cpymem(errstr, msg->data, size);
}


/**
 * 初始化数组ngx_sys_errlist
 *
 * 把系统错误码全部拷贝到ngx_sys_errlist数组中
 */
ngx_int_t
ngx_strerror_init(void)
{
    char       *msg;
    u_char     *p;
    size_t      len;
    ngx_err_t   err;

    /*
     * ngx_strerror() is not ready to work at this stage, therefore,
     * malloc() is used and possible errors are logged using strerror().
     */

    // 计算存储ngx错误类型需要的内存,这里最多NGX_SYS_NERR个错误类型可以被记录
    len = NGX_SYS_NERR * sizeof(ngx_str_t);

    ngx_sys_errlist = malloc(len);
    if (ngx_sys_errlist == NULL) {
        goto failed;
    }

    // NGX_SYS_NERR代表系统错误编号个数
    for (err = 0; err < NGX_SYS_NERR; err++) {
    	// 根据标准错误号,获取错误的描述字符
        msg = strerror(err);
        len = ngx_strlen(msg);

        p = malloc(len);
        if (p == NULL) {
            goto failed;
        }

        ngx_memcpy(p, msg, len);
        ngx_sys_errlist[err].len = len; // 错误内容长度
        ngx_sys_errlist[err].data = p; // 错误内容
    }

    return NGX_OK;

failed:

    err = errno;
    ngx_log_stderr(0, "malloc(%uz) failed (%d: %s)", len, err, strerror(err));

    return NGX_ERROR;
}
