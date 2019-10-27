
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_PALLOC_H_INCLUDED_
#define _NGX_PALLOC_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


/*
 * NGX_MAX_ALLOC_FROM_POOL should be (ngx_pagesize - 1), i.e. 4095 on x86.
 * On Windows NT it decreases a number of locked pages in a kernel.
 */
#define NGX_MAX_ALLOC_FROM_POOL  (ngx_pagesize - 1)

#define NGX_DEFAULT_POOL_SIZE    (16 * 1024)

#define NGX_POOL_ALIGNMENT       16
#define NGX_MIN_POOL_SIZE                                                     \
    ngx_align((sizeof(ngx_pool_t) + 2 * sizeof(ngx_pool_large_t)),            \
              NGX_POOL_ALIGNMENT)


typedef void (*ngx_pool_cleanup_pt)(void *data);

typedef struct ngx_pool_cleanup_s  ngx_pool_cleanup_t;

struct ngx_pool_cleanup_s {
    ngx_pool_cleanup_pt   handler;
    //调用handler方法时传入的数据
    void                 *data;
    ngx_pool_cleanup_t   *next;
};


typedef struct ngx_pool_large_s  ngx_pool_large_t;

struct ngx_pool_large_s {
    ngx_pool_large_t     *next;
    void                 *alloc;
};


//pool中用于实际存放内存的数据结构
typedef struct {
    //指向未被分配出去地址的首地址,如果将来需要使用这个data块分配内存,
    //则last指向的地址，就是分配出去地址的首地址
    u_char               *last;
    //当着这个pool_data的边界地址;比如创建pool时指定的size是10,地址从0开始,
    //那么整个pool_data就会管理1~9的地址,end则等于10
    u_char               *end;
    //指向下一个pool
    ngx_pool_t           *next;
    //分配内存时失败次数
    ngx_uint_t            failed;
} ngx_pool_data_t;


struct ngx_pool_s {
    // 实际存放数据的地方
    ngx_pool_data_t       d;
    // 每个ngx_pool_data_t可容纳的空间大小
    size_t                max;
    // 在整个pool链中,当前正在被使用pool
    ngx_pool_t           *current;
    /*
     * 一个空闲链,这个链里面存放了被释放的链项
     * 当释放掉不需要的链的时候,ngx并没有将其销毁,而是放在了chain字段中,以后再需要分配ngx_chain_t
     *
     * 获取chain的时候可以优先从这个chain中获取，但是这个chain里的buf是不能用的，使用的时候要认为这里没有buf
     */
    ngx_chain_t          *chain;
    // 同样是个单链表,当ngx_pool_data_t的最大容量都无法满足时使用
    ngx_pool_large_t     *large;
    // 该结构可以用来注册一些方法,当整个pool销毁时用到
    ngx_pool_cleanup_t   *cleanup;
    ngx_log_t            *log;
};


// 关闭fd用的数据结构
typedef struct {
    //文件描述符
    ngx_fd_t              fd;
    //文件名字
    u_char               *name;
    ngx_log_t            *log;
} ngx_pool_cleanup_file_t;


/*对系统函数malloc()简单的封装,不会初始化分配的内存*/
void *ngx_alloc(size_t size, ngx_log_t *log);
/*和ngx_alloc功能一样,用来分配内存,但并不是对系统函数calloc()的封装
 *而是分配内存后使用函数memset()对已分配的内存进行初始化*/
void *ngx_calloc(size_t size, ngx_log_t *log);


//创建一个ngx_pool_t结构体
ngx_pool_t *ngx_create_pool(size_t size, ngx_log_t *log);
//销毁pool这个结构体
void ngx_destroy_pool(ngx_pool_t *pool);
//重置pool,相当于清空pool中的所有数据
void ngx_reset_pool(ngx_pool_t *pool);

//从*pool中分配size大小的字节,并对齐地址
void *ngx_palloc(ngx_pool_t *pool, size_t size);
//从*pool中分配size大小的字节,不对齐地址
void *ngx_pnalloc(ngx_pool_t *pool, size_t size);
//从*pool中分配size大小的字节,对齐地址并对内存初始化
void *ngx_pcalloc(ngx_pool_t *pool, size_t size);
//返回一个按照alignment字节数对齐的size大小的字节地址
void *ngx_pmemalign(ngx_pool_t *pool, size_t size, size_t alignment);
//释放存放在pool->large链表中的*p内存
ngx_int_t ngx_pfree(ngx_pool_t *pool, void *p);


//向pool->cleanup链末尾添加一个ngx_pool_cleanup_t对象
ngx_pool_cleanup_t *ngx_pool_cleanup_add(ngx_pool_t *p, size_t size);
//在*p->cleanup中寻找指定的fd并将其关闭
void ngx_pool_run_cleanup_file(ngx_pool_t *p, ngx_fd_t fd);
//关闭*data结构(ngx_pool_cleanup_file_t)中的fd
void ngx_pool_cleanup_file(void *data);
//删除*data结构(ngx_pool_cleanup_file_t)中名字为*name的文件,然后在关闭该结构中的fd
void ngx_pool_delete_file(void *data);


#endif /* _NGX_PALLOC_H_INCLUDED_ */
