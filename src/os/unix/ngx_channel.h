
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_CHANNEL_H_INCLUDED_
#define _NGX_CHANNEL_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


/*
 * 主进程和子进程通信的一个结构体
 */
typedef struct {
	 // 携带的命令
     ngx_uint_t  command;
     // 对端进程id
     ngx_pid_t   pid;
     // 对端进程在ngx_processes中的位置
     ngx_int_t   slot;
     /*
      * 用来和对端通信的文件描述符
      * 主进程向channel[0]中写
      * 子进程向channel[1]中读
      */
     ngx_fd_t    fd;
} ngx_channel_t;


ngx_int_t ngx_write_channel(ngx_socket_t s, ngx_channel_t *ch, size_t size,
    ngx_log_t *log);
ngx_int_t ngx_read_channel(ngx_socket_t s, ngx_channel_t *ch, size_t size,
    ngx_log_t *log);
ngx_int_t ngx_add_channel_event(ngx_cycle_t *cycle, ngx_fd_t fd,
    ngx_int_t event, ngx_event_handler_pt handler);
void ngx_close_channel(ngx_fd_t *fd, ngx_log_t *log);


#endif /* _NGX_CHANNEL_H_INCLUDED_ */
