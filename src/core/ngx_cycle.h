
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/**
   四个星号解释 ****conf_ctx
   根据宏定义ngx_event_get_conf来解释
   设ctx = conf_ctx

    ctx
	-----
	| * |
	-----
	|	    	|(ngx_events_module.index)
	\*(ctx+0)   \*(ctx+index)  设*(ctx+index)为index
	 ----------------------
     | * | .. |  *  |这一层内存有main中调用ngx_init_cycle函数创建,共ngx_max_module个。 cycle->conf_ctx[ngx_events_module.index] 等于*(ctx+index)的值。
	 ----------------------
	 	 	   |*(index+0)
	 	 	   \设*(index+0)为index0
	  	  	    ---------
	  	  	    |   *   | 这一层指针有具体的核心模块负责创建。  *(cycle->conf_ctx[ngx_events_module.index]) 等于 *( *(ctx+index) + 0 ) 的值。
	  	  	    ---------
	  	  	    |*(index0 + 0)				 		   |*(index0 + ctx_index)
	  	  	    |存放event_core事件模块用的的结构体的地址     |存放epoll事件模块用到的配置结构体的地址
	  	  	    \设*(index0+0)为index00		 			\设*(index0 + ctx_index)为index0ctx_index
	   	   	   	 ------------------------------------------------------------
	   	   	   	 |  		   *             |  				*  			|这一层存放自定义模块的配置结构体地址。(*(cycle->conf_ctx[ngx_events_module.index]))[ngx_epoll_module.ctx_index] 等于 *( (*( *(ctx+index) + 0 )) + ctx_index)的值。
	   	   	   	 ------------------------------------------------------------
				 |							  	 |存放ngx_epoll_conf_t结构体的数据
				 \*(index00 + 0)				 \ *(index0ctx_index + 0)
		 	 	  ------------------	  		  --------------------
  	  	 	 	  |ngx_event_conf_t|	  		  | ngx_epoll_conf_t |   等于 *( ( *( ( *( *(ctx+index) + 0 ) ) + ctx_index)) + 0 )的值。
		 	 	  ------------------	  		  --------------------
 */



#ifndef _NGX_CYCLE_H_INCLUDED_
#define _NGX_CYCLE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#ifndef NGX_CYCLE_POOL_SIZE
#define NGX_CYCLE_POOL_SIZE     NGX_DEFAULT_POOL_SIZE
#endif


#define NGX_DEBUG_POINTS_STOP   1
#define NGX_DEBUG_POINTS_ABORT  2


typedef struct ngx_shm_zone_s  ngx_shm_zone_t;

typedef ngx_int_t (*ngx_shm_zone_init_pt) (ngx_shm_zone_t *zone, void *data);

struct ngx_shm_zone_s {
    void                     *data;
    ngx_shm_t                 shm;
    ngx_shm_zone_init_pt      init;
    void                     *tag;
    ngx_uint_t                noreuse;  /* unsigned  noreuse:1; */
};


struct ngx_cycle_s {
	// 所有(核心?)模块的配置结构体指针
	// 在二级指针的第7个位置是http核心模块的配置结构体指针ngx_http_conf_ctx_t
	// ngx_http_conf_ctx_t中的main_conf的第二级指针存放的是所有http模块的main级别的自定义结构体
    void                  ****conf_ctx;
    ngx_pool_t               *pool;

    ngx_log_t                *log;
    ngx_log_t                 new_log;

    ngx_uint_t                log_use_stderr;  /* unsigned  log_use_stderr:1; */

    // 一个数组,存放正在使用的连接(ngx_connection_t)
    // 下标就是socket文件描述符
    // 在ngx_event_process_init方法中分配内存空间(分配files_n个指针空间)
    // 在ngx_get_connection方法中具体赋值
    ngx_connection_t        **files;
    // 指向connections数组中的一个空闲连接
    ngx_connection_t         *free_connections;
    // 空闲连接的个数
    ngx_uint_t                free_connection_n;

    ngx_queue_t               reusable_connections_queue;

    ngx_array_t               listening;
    ngx_array_t               paths;
    ngx_array_t               config_dump;
    ngx_list_t                open_files;
    ngx_list_t                shared_memory;

    // 可创建连接的最大个数,也就是connections数组的大小
    ngx_uint_t                connection_n;
    // 当前进程可以打开的最大描述符个数
    ngx_uint_t                files_n;

    // connections本身是一个数组,但是数组里面每一个ngx_connection_t对象又有一个指向
    // 下一个空闲ngx_connection_t对象的指针(使用data字段临时代替)
    ngx_connection_t         *connections;
    // 每个连接对应的读事件对象
    ngx_event_t              *read_events;
    // 每个连接对应的写事件对象
    ngx_event_t              *write_events;

    ngx_cycle_t              *old_cycle;

    // 配置文件nginx.conf的绝对路劲
    ngx_str_t                 conf_file;
    ngx_str_t                 conf_param;
    // 配置文件nginx.conf所在的目录
    ngx_str_t                 conf_prefix;
    // 安装路径
    ngx_str_t                 prefix;
    ngx_str_t                 lock_file;
    ngx_str_t                 hostname;
};


typedef struct {
     ngx_flag_t               daemon;

     /* 对应master_process指令,表示是否开启master模式(也就是master-worker模拟式),默认开启
      * 如果值为零则表示不开启,也就是单进程模式(开发调试时使用)。
      */
     ngx_flag_t               master;

     ngx_msec_t               timer_resolution;

     ngx_int_t                worker_processes;
     ngx_int_t                debug_points;

     ngx_int_t                rlimit_nofile;
     off_t                    rlimit_core;

     int                      priority;

     ngx_uint_t               cpu_affinity_n;
     uint64_t                *cpu_affinity;

     char                    *username;
     ngx_uid_t                user;
     ngx_gid_t                group;

     ngx_str_t                working_directory;
     ngx_str_t                lock_file;

     ngx_str_t                pid;
     ngx_str_t                oldpid;

     ngx_array_t              env;
     char                   **environment;
} ngx_core_conf_t;


#define ngx_is_init_cycle(cycle)  (cycle->conf_ctx == NULL)


ngx_cycle_t *ngx_init_cycle(ngx_cycle_t *old_cycle);
ngx_int_t ngx_create_pidfile(ngx_str_t *name, ngx_log_t *log);
void ngx_delete_pidfile(ngx_cycle_t *cycle);
ngx_int_t ngx_signal_process(ngx_cycle_t *cycle, char *sig);
void ngx_reopen_files(ngx_cycle_t *cycle, ngx_uid_t user);
char **ngx_set_environment(ngx_cycle_t *cycle, ngx_uint_t *last);
ngx_pid_t ngx_exec_new_binary(ngx_cycle_t *cycle, char *const *argv);
uint64_t ngx_get_cpu_affinity(ngx_uint_t n);
ngx_shm_zone_t *ngx_shared_memory_add(ngx_conf_t *cf, ngx_str_t *name,
    size_t size, void *tag);


extern volatile ngx_cycle_t  *ngx_cycle;
extern ngx_array_t            ngx_old_cycles;
extern ngx_module_t           ngx_core_module;
extern ngx_uint_t             ngx_test_config;
extern ngx_uint_t             ngx_dump_config;
extern ngx_uint_t             ngx_quiet_mode;


#endif /* _NGX_CYCLE_H_INCLUDED_ */
