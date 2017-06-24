
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_PROCESS_H_INCLUDED_
#define _NGX_PROCESS_H_INCLUDED_


#include <ngx_setaffinity.h>
#include <ngx_setproctitle.h>


typedef pid_t       ngx_pid_t;

#define NGX_INVALID_PID  -1

typedef void (*ngx_spawn_proc_pt) (ngx_cycle_t *cycle, void *data);

typedef struct {
	// 进程id
    ngx_pid_t           pid;
    int                 status;
    /*
     * 主进程和子进程通信所用的通道
     * 	channel[0]主进程中文件描述符
     * 	channel[1]子进程中文件描述符
     *
     * 对于在ngx_processes[]数组中位置相同的元素,不关它处于哪个worekr进程中,他们代表的
     * worker是相同的,比如woker0中的ngx_processes[5]和worker2中的ngx_processes[5]
     * 都代表worker5,他们的pid都是相同的,但是channel[0]的值不一定相同。
     *
     * 上面说到,channel[0]是主进程中的文件描述符,更准确的描述应该是主进程描述符指向的那个"文件"(文件指针)
     * 主进程在创建完子进程后,后续主进程创建的文件描述符子进程是感知不到的,为了让子进程能够感知到
     * 父进程创建的"文件",ngx利用匿名Unix域套接字来实现(即socketpair()和sendmsg/recvmsg),主进程的文件
     * 指针向子进程的传递,比如在主进程中fd等于5,而到子进程中fd可能就等于6,但是他们指向同一个"文件"。
     */
    ngx_socket_t        channel[2];

    /*
     * worker工作循环方法
     * ngx_worker_process_cycle
     */
    ngx_spawn_proc_pt   proc;

    // 进程编号?
    void               *data;

    // 子进程名字
    char               *name;

    // 在主进程的ngx_processes数组中该变量都是1 TODO
    unsigned            respawn:1;

    /*
     * 1表示进程是刚fork出来的(比如reload的时候)ngx_start_worker_processes方法
     * 不会向该标志位为1的进程发送reload信号,判断完该标志位后就会将它置为0
     */
    unsigned            just_spawn:1;
    unsigned            detached:1;
    // 1表示进程正在退出;
    unsigned            exiting:1;
    // 1表示进程已经退出;0进程没有退出
    unsigned            exited:1;
} ngx_process_t;


typedef struct {
    char         *path;
    char         *name;
    char *const  *argv;
    char *const  *envp;
} ngx_exec_ctx_t;


#define NGX_MAX_PROCESSES         1024

#define NGX_PROCESS_NORESPAWN     -1
#define NGX_PROCESS_JUST_SPAWN    -2
#define NGX_PROCESS_RESPAWN       -3
#define NGX_PROCESS_JUST_RESPAWN  -4

// 二进制升级用? TODO
#define NGX_PROCESS_DETACHED      -5


#define ngx_getpid   getpid

#ifndef ngx_log_pid
#define ngx_log_pid  ngx_pid
#endif


ngx_pid_t ngx_spawn_process(ngx_cycle_t *cycle,
    ngx_spawn_proc_pt proc, void *data, char *name, ngx_int_t respawn);
ngx_pid_t ngx_execute(ngx_cycle_t *cycle, ngx_exec_ctx_t *ctx);
ngx_int_t ngx_init_signals(ngx_log_t *log);
void ngx_debug_point(void);


#if (NGX_HAVE_SCHED_YIELD)
#define ngx_sched_yield()  sched_yield()
#else
#define ngx_sched_yield()  usleep(1)
#endif


extern int            ngx_argc;
extern char         **ngx_argv;
extern char         **ngx_os_argv;

extern ngx_pid_t      ngx_pid;
extern ngx_socket_t   ngx_channel;
extern ngx_int_t      ngx_process_slot;
extern ngx_int_t      ngx_last_process;
extern ngx_process_t  ngx_processes[NGX_MAX_PROCESSES];


#endif /* _NGX_PROCESS_H_INCLUDED_ */
