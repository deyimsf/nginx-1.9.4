
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_ERRNO_H_INCLUDED_
#define _NGX_ERRNO_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef int               ngx_err_t;

#define NGX_EPERM         EPERM
#define NGX_ENOENT        ENOENT
#define NGX_ENOPATH       ENOENT

// No such process
#define NGX_ESRCH         ESRCH

/**
 * 该信号表示中断了一个阻塞的系统调用
 *
 * 一个阻塞的系统调用(如epoll_wait)可以被信号中断,中断发生在信号绑定的函数执行完毕之后。
 * 如果这个这个信号在绑定函数的时候,指定了SA_RESTART标志位,那么有些阻塞调用时可以被重启的,
 * 比如read、write等函数。有些阻塞函数(如epoll_wait)即使信号指定了SA_RESTART标志位,也
 * 无法重启。
 *
 */
#define NGX_EINTR         EINTR

// No child processes (POSIX.1)
#define NGX_ECHILD        ECHILD
#define NGX_ENOMEM        ENOMEM
#define NGX_EACCES        EACCES
#define NGX_EBUSY         EBUSY
#define NGX_EEXIST        EEXIST
#define NGX_EXDEV         EXDEV
#define NGX_ENOTDIR       ENOTDIR
#define NGX_EISDIR        EISDIR
#define NGX_EINVAL        EINVAL

/*
 * Too many open files in system (POSIX.1).  On Linux,
 * this is probably a result of encountering the /proc/sys/fs/file-max limit
 */
#define NGX_ENFILE        ENFILE

// Too many open files (POSIX.1).
#define NGX_EMFILE        EMFILE
#define NGX_ENOSPC        ENOSPC
#define NGX_EPIPE         EPIPE
#define NGX_EINPROGRESS   EINPROGRESS
#define NGX_ENOPROTOOPT   ENOPROTOOPT
#define NGX_EOPNOTSUPP    EOPNOTSUPP
#define NGX_EADDRINUSE    EADDRINUSE

// Connection aborted (POSIX.1)
#define NGX_ECONNABORTED  ECONNABORTED
#define NGX_ECONNRESET    ECONNRESET
#define NGX_ENOTCONN      ENOTCONN
#define NGX_ETIMEDOUT     ETIMEDOUT
#define NGX_ECONNREFUSED  ECONNREFUSED
#define NGX_ENAMETOOLONG  ENAMETOOLONG
#define NGX_ENETDOWN      ENETDOWN
#define NGX_ENETUNREACH   ENETUNREACH
#define NGX_EHOSTDOWN     EHOSTDOWN
#define NGX_EHOSTUNREACH  EHOSTUNREACH
#define NGX_ENOSYS        ENOSYS
#define NGX_ECANCELED     ECANCELED
#define NGX_EILSEQ        EILSEQ
#define NGX_ENOMOREFILES  0
#define NGX_ELOOP         ELOOP
#define NGX_EBADF         EBADF

#if (NGX_HAVE_OPENAT)
#define NGX_EMLINK        EMLINK
#endif

#if (__hpux__)
#define NGX_EAGAIN        EWOULDBLOCK
#else
// non-blocking and interrupt i/o
#define NGX_EAGAIN        EAGAIN
#endif


#define ngx_errno                  errno
#define ngx_socket_errno           errno
#define ngx_set_errno(err)         errno = err
#define ngx_set_socket_errno(err)  errno = err


u_char *ngx_strerror(ngx_err_t err, u_char *errstr, size_t size);
ngx_int_t ngx_strerror_init(void);


#endif /* _NGX_ERRNO_H_INCLUDED_ */
