
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * ngx中使用cycle->shared_memory数组来收集共享内存配置信息,数组中的一个元素代表一个共享内存的配置,用
 * ngx_shm_zone_s结构体表示:
 *    struct ngx_shm_zone_s {
 *       void                     *data;
 *       ngx_shm_t                 shm;
 *       ngx_shm_zone_init_pt      init;
 *       void                     *tag;
 *       ngx_uint_t                noreuse;
 *    }
 * 为了方便向cycle->shared_memory数组中添加共享内存配置信息,ngx提供了一个ngx_shared_memory_add()方法
 *
 * 在ngx_init_cycle()方法中会遍历cycle->shared_memory数组中的ngx_shm_zone_s结构体,根据里面的信息
 *   1.通过ngx_shm_alloc()方法来创建共享内存
 *   2.通过ngx_init_zone_pool()方法将共享内存前sizeof(ngx_slab_pool_t)个字节看成ngx_slab_pool_t对象并初始化
 *   3.回调shm_zone.init()方法
 * 在上面的第2步可以看到,添加到cycle->shared_memory数组中的共享内存大小配信息不应该小于sizeof(ngx_slab_pool_t)个字节
 * 实际上大部分模块都有一个最小限制(8个ngx_pagesize大小的字节,linux中pagesize一般为4096字节)
 *
 * ngx使用core/ngx_slab.c中的方法来管理共享内存,比如使用ngx_slab_alloc()方法来分配内存
 *
 */

#ifndef _NGX_SHMEM_H_INCLUDED_
#define _NGX_SHMEM_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


/**
 * 共享内存
 */
typedef struct {
	// 共享内存的内存地址
    u_char      *addr;
    // 共享内存大小
    size_t       size;
    // 共享内存名字
    ngx_str_t    name;
    ngx_log_t   *log;
    ngx_uint_t   exists;   /* unsigned  exists:1;  */
} ngx_shm_t;


ngx_int_t ngx_shm_alloc(ngx_shm_t *shm);
void ngx_shm_free(ngx_shm_t *shm);


#endif /* _NGX_SHMEM_H_INCLUDED_ */
