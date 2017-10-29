
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_CORE_H_INCLUDED_
#define _NGX_CORE_H_INCLUDED_


#include <ngx_config.h>


typedef struct ngx_module_s      ngx_module_t;
typedef struct ngx_conf_s        ngx_conf_t;
typedef struct ngx_cycle_s       ngx_cycle_t;
typedef struct ngx_pool_s        ngx_pool_t;
typedef struct ngx_chain_s       ngx_chain_t;
typedef struct ngx_log_s         ngx_log_t;
typedef struct ngx_open_file_s   ngx_open_file_t;
typedef struct ngx_command_s     ngx_command_t;
typedef struct ngx_file_s        ngx_file_t;
typedef struct ngx_event_s       ngx_event_t;
typedef struct ngx_event_aio_s   ngx_event_aio_t;
typedef struct ngx_connection_s  ngx_connection_t;

#if (NGX_THREADS)
typedef struct ngx_thread_task_s  ngx_thread_task_t;
#endif

typedef void (*ngx_event_handler_pt)(ngx_event_t *ev);
typedef void (*ngx_connection_handler_pt)(ngx_connection_t *c);


/*
 * 以下返回码在不同的功能中有不同的意义,对于大部分功能其意义如下
 *  NGX_OK: Operation succeeded.(操作成功)
 *  NGX_ERROR: Operation failed.(操作失败)
 *  NGX_AGAIN: Operation incomplete; call the function again.(操作未完成,会被再次调用)
 *  NGX_DECLINED: Operation rejected, for example, because it is disabled in the configuration.
 * 		This is never an error.(操作被拒绝. 比如它在配置中是被禁用的,但它绝对不是一个错误.)
 *  NGX_BUSY: Resource is not available.(没有可用的资源.)
 *  NGX_DONE: Operation complete or continued elsewhere. Also used as an alternative success code.
 *  NGX_ABORT: Function was aborted. Also used as an alternative error code.
 *
 *
 * 在阶段handler中这个返回值具体的意义如下:
 *  NGX_OK: 注册的handler方法返回这个表示本阶段已经处理完毕,跳到下一个阶段.
 *  NGX_DECLINED: 去执行这个阶段的下一个方法,如果当前handler方法已经是当前阶段的最后一个,那就去执行下一个阶段.
 *  NGX_AGAIN, NGX_DONE: 暂定本handler执行,暂定的原因可以是一个异步I/O操作或者仅仅一个delay等
 *  除了以上几个返回码,handler返回的其它任何码都被认为是这个请求的终结码,特别是http响应码(304、302等)
 *
 * 有些阶段的返回码和上面的稍有不同:
 *  内容阶段(content phase)里面的handler(处理内容的handler)返回码,除了NGX_DECLINED都被视为请求终结码
 *
 *  访问阶段(access phase),in satisfy any mode, any return code other than NGX_OK, NGX_DECLINED,
 *  NGX_AGAIN, NGX_DONE is considered a denial. If no subsequent access handlers allow or deny
 *  access with a different code, the denial code will become the finalization code.
 *
 *
 * 其它方法
 *
 */
#define  NGX_OK          0
#define  NGX_ERROR      -1
#define  NGX_AGAIN      -2
#define  NGX_BUSY       -3
#define  NGX_DONE       -4
#define  NGX_DECLINED   -5
#define  NGX_ABORT      -6


#include <ngx_errno.h>
#include <ngx_atomic.h>
#include <ngx_thread.h>
#include <ngx_rbtree.h>
#include <ngx_time.h>
#include <ngx_socket.h>
#include <ngx_string.h>
#include <ngx_files.h>
#include <ngx_shmem.h>
#include <ngx_process.h>
#include <ngx_user.h>
#include <ngx_parse.h>
#include <ngx_parse_time.h>
#include <ngx_log.h>
#include <ngx_alloc.h>
#include <ngx_palloc.h>
#include <ngx_buf.h>
#include <ngx_queue.h>
#include <ngx_array.h>
#include <ngx_list.h>
#include <ngx_hash.h>
#include <ngx_file.h>
#include <ngx_crc.h>
#include <ngx_crc32.h>
#include <ngx_murmurhash.h>
#if (NGX_PCRE)
#include <ngx_regex.h>
#endif
#include <ngx_radix_tree.h>
#include <ngx_times.h>
#include <ngx_rwlock.h>
#include <ngx_shmtx.h>
#include <ngx_slab.h>
#include <ngx_inet.h>
#include <ngx_cycle.h>
#include <ngx_resolver.h>
#if (NGX_OPENSSL)
#include <ngx_event_openssl.h>
#endif
#include <ngx_process_cycle.h>
#include <ngx_conf_file.h>
#include <ngx_open_file_cache.h>
#include <ngx_os.h>
#include <ngx_connection.h>
#include <ngx_syslog.h>
#include <ngx_proxy_protocol.h>


#define LF     (u_char) '\n'
#define CR     (u_char) '\r'
#define CRLF   "\r\n"


#define ngx_abs(value)       (((value) >= 0) ? (value) : - (value))
#define ngx_max(val1, val2)  ((val1 < val2) ? (val2) : (val1))
#define ngx_min(val1, val2)  ((val1 > val2) ? (val2) : (val1))

void ngx_cpuinfo(void);

#if (NGX_HAVE_OPENAT)
#define NGX_DISABLE_SYMLINKS_OFF        0
#define NGX_DISABLE_SYMLINKS_ON         1
#define NGX_DISABLE_SYMLINKS_NOTOWNER   2
#endif

#endif /* _NGX_CORE_H_INCLUDED_ */
