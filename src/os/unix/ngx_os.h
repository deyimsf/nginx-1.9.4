
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_OS_H_INCLUDED_
#define _NGX_OS_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#define NGX_IO_SENDFILE    1


typedef ssize_t (*ngx_recv_pt)(ngx_connection_t *c, u_char *buf, size_t size);
typedef ssize_t (*ngx_recv_chain_pt)(ngx_connection_t *c, ngx_chain_t *in,
    off_t limit);
typedef ssize_t (*ngx_send_pt)(ngx_connection_t *c, u_char *buf, size_t size);
typedef ngx_chain_t *(*ngx_send_chain_pt)(ngx_connection_t *c, ngx_chain_t *in,
    off_t limit);

typedef struct {
	// 读方法
    ngx_recv_pt        recv;
    ngx_recv_chain_pt  recv_chain;
    ngx_recv_pt        udp_recv;
    // 写方法
    ngx_send_pt        send;
    ngx_send_chain_pt  send_chain;
    ngx_uint_t         flags;
} ngx_os_io_t;


ngx_int_t ngx_os_init(ngx_log_t *log);
void ngx_os_status(ngx_log_t *log);
ngx_int_t ngx_os_specific_init(ngx_log_t *log);
void ngx_os_specific_status(ngx_log_t *log);
ngx_int_t ngx_daemon(ngx_log_t *log);
ngx_int_t ngx_os_signal_process(ngx_cycle_t *cycle, char *sig, ngx_int_t pid);


ssize_t ngx_unix_recv(ngx_connection_t *c, u_char *buf, size_t size);
ssize_t ngx_readv_chain(ngx_connection_t *c, ngx_chain_t *entry, off_t limit);
ssize_t ngx_udp_unix_recv(ngx_connection_t *c, u_char *buf, size_t size);
ssize_t ngx_unix_send(ngx_connection_t *c, u_char *buf, size_t size);
ngx_chain_t *ngx_writev_chain(ngx_connection_t *c, ngx_chain_t *in,
    off_t limit);


#if (IOV_MAX > 64)
#define NGX_IOVS_PREALLOCATE  64
#else
#define NGX_IOVS_PREALLOCATE  IOV_MAX
#endif


/*
 * 用来映射ngx_chain_t中数据的结构体
 */
typedef struct {
    struct iovec  *iovs;

    /*
     * 映射的链表项用到的struct iovec结构体的个数
     *
     * nalloc相当于 iovs数组的最大容量
     * count相当于 iovs数组已使用的容量
     */
    ngx_uint_t     count;

    // 总共映射的内存大小
    size_t         size;

    // iovs数组的个数,是对iovs数组的一个限制,最多就nalloc个
    ngx_uint_t     nalloc;
} ngx_iovec_t;

ngx_chain_t *ngx_output_chain_to_iovec(ngx_iovec_t *vec, ngx_chain_t *in,
    size_t limit, ngx_log_t *log);


ssize_t ngx_writev(ngx_connection_t *c, ngx_iovec_t *vec);


extern ngx_os_io_t  ngx_os_io;
extern ngx_int_t    ngx_ncpu;
extern ngx_int_t    ngx_max_sockets;
extern ngx_uint_t   ngx_inherited_nonblocking;
extern ngx_uint_t   ngx_tcp_nodelay_and_tcp_nopush;


#if (NGX_FREEBSD)
#include <ngx_freebsd.h>


#elif (NGX_LINUX)
#include <ngx_linux.h>


#elif (NGX_SOLARIS)
#include <ngx_solaris.h>


#elif (NGX_DARWIN)
#include <ngx_darwin.h>
#endif


#endif /* _NGX_OS_H_INCLUDED_ */
