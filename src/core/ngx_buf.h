
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_BUF_H_INCLUDED_
#define _NGX_BUF_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef void *            ngx_buf_tag_t;

typedef struct ngx_buf_s  ngx_buf_t;

struct ngx_buf_s {
	/*
	 * pos和last代表实际使用的内存大小。
	 *
	 * pos >= start
	 * last <= end
	 *
	 * 从缓存读内容,则增加pos值就可以,但是不能大于end
	 * 向缓存写数据,则增加last值就可以,但是不能大于end
	 *
	 * 半开集的使用关系 [pos,last)
	 */
    u_char          *pos;
    u_char          *last;
    off_t            file_pos;
    off_t            file_last;

    /*
     * start和end代表缓存实际上分配的内存大小,跟有没有使用没有关系
     * 半开集的使用关系 [start,end)
     */
    u_char          *start;         /* start of buffer */
    u_char          *end;           /* end of buffer */
    // 一个tag,可以用来表示用一种buf
    ngx_buf_tag_t    tag;
    ngx_file_t      *file;
    ngx_buf_t       *shadow;


    /* the buf's content could be changed */
    // 为1表示该缓存内容可以被改变
    unsigned         temporary:1;

    /*
     * the buf's content is in a memory cache or in a read only memory
     * and must not be changed
     */
    unsigned         memory:1;

    /* the buf's content is mmap()ed and must not be changed */
    unsigned         mmap:1;

    unsigned         recycled:1;
    // 1表示处理的是文件 TODO
    unsigned         in_file:1;
    // 1表示需要进行刷新操作 TODO
    unsigned         flush:1;
    // 1表示需要进行同步操作 TODO
    unsigned         sync:1;

    /*
     * 一个完整的响应数据在传递给filter的时候,有可能会被分隔成多个链(ngx_chain_t,表示一个链,里面有很多链项)。
     * 假设数据在输出时被切割成了2个链进行传递,并且每个链包含了3个链项,那么
     * 	last_buf=1 表示该buf是整个响应数据的最后一个块缓存
     * 	last_in_chain=1 表示该buf只是某一个链的最后一块缓存
     * 有次可以知道,last_buf一定是last_in_chain,但是last_in_chain不一定是last_buf.
     *
     * 画图举例
     *  第一个链:
     *		  	   ngx_chain_t						     ngx_chain_t						  ngx_chain_t
     *			----------------					  ----------------						----------------
     *			| *buf | *next | ------------------>  | *buf | *next | ------------------>	| *buf | *next |
     *			----------------					  ----------------						----------------
     *			0	/       0						 0   /		   0						0   /		   1
     *     ----------------------------			  ----------------------------			----------------------------
     *     | last_buf | last_in_chain |			  | last_buf | last_in_chain |			| last_buf | last_in_chain |
     *	   ----------------------------			  ----------------------------			----------------------------
     *
     *  第二个链:
     *		  	   ngx_chain_t						     ngx_chain_t						  ngx_chain_t
     *			----------------					  ----------------						----------------
     *			| *buf | *next | ------------------>  | *buf | *next | ------------------>	| *buf | *next |
     *			----------------					  ----------------						----------------
     *			0	/       0						 0   /		   0						1  /		   1
     *     ----------------------------			  ----------------------------			----------------------------
     *     | last_buf | last_in_chain |			  | last_buf | last_in_chain |			| last_buf | last_in_chain |
     *	   ----------------------------			  ----------------------------			----------------------------
     *
     */
    unsigned         last_buf:1;
    unsigned         last_in_chain:1;

    unsigned         last_shadow:1;
    unsigned         temp_file:1;

    /* STUB */ int   num;
};


struct ngx_chain_s {
    ngx_buf_t    *buf;
    ngx_chain_t  *next;
};


/*
 * 用来批量创建ngx_buf_t结构体
 *
 * ngx_create_chain_of_bufs()方法用
 */
typedef struct {
	// 要创建的ngx_buf_t结构体的个数
    ngx_int_t    num;
    // 每个ngx_buf_t管控的内存大小
    size_t       size;
} ngx_bufs_t;


typedef struct ngx_output_chain_ctx_s  ngx_output_chain_ctx_t;

typedef ngx_int_t (*ngx_output_chain_filter_pt)(void *ctx, ngx_chain_t *in);

#if (NGX_HAVE_FILE_AIO)
typedef void (*ngx_output_chain_aio_pt)(ngx_output_chain_ctx_t *ctx,
    ngx_file_t *file);
#endif

struct ngx_output_chain_ctx_s {
    ngx_buf_t                   *buf;
    ngx_chain_t                 *in;
    ngx_chain_t                 *free;
    ngx_chain_t                 *busy;

    /*
     * 如果操作系统支持sendfile方法,并且ngx开启了sendfile命令则该值为1
     *
     */
    unsigned                     sendfile:1;
    unsigned                     directio:1;
#if (NGX_HAVE_ALIGNED_DIRECTIO)
    unsigned                     unaligned:1;
#endif
    unsigned                     need_in_memory:1;
    unsigned                     need_in_temp:1;
#if (NGX_HAVE_FILE_AIO || NGX_THREADS)
    unsigned                     aio:1;
#endif

#if (NGX_HAVE_FILE_AIO)
    ngx_output_chain_aio_pt      aio_handler;
#if (NGX_HAVE_AIO_SENDFILE)
    ssize_t                    (*aio_preload)(ngx_buf_t *file);
#endif
#endif

#if (NGX_THREADS)
    ngx_int_t                  (*thread_handler)(ngx_thread_task_t *task,
                                                 ngx_file_t *file);
    ngx_thread_task_t           *thread_task;
#endif

    off_t                        alignment;

    ngx_pool_t                  *pool;
    ngx_int_t                    allocated;
    ngx_bufs_t                   bufs;
    ngx_buf_tag_t                tag;

    ngx_output_chain_filter_pt   output_filter;
    void                        *filter_ctx;
};


typedef struct {
    ngx_chain_t                 *out;
    ngx_chain_t                **last;
    ngx_connection_t            *connection;
    ngx_pool_t                  *pool;
    off_t                        limit;
} ngx_chain_writer_ctx_t;


#define NGX_CHAIN_ERROR     (ngx_chain_t *) NGX_ERROR


#define ngx_buf_in_memory(b)        (b->temporary || b->memory || b->mmap)
#define ngx_buf_in_memory_only(b)   (ngx_buf_in_memory(b) && !b->in_file)

/**
 * TODO 特殊buf
 */
#define ngx_buf_special(b)                                                   \
    ((b->flush || b->last_buf || b->sync)                                    \
     && !ngx_buf_in_memory(b) && !b->in_file)

#define ngx_buf_sync_only(b)                                                 \
    (b->sync                                                                 \
     && !ngx_buf_in_memory(b) && !b->in_file && !b->flush && !b->last_buf)

/*
 * 计算buf中缓存的数据大小
 */
#define ngx_buf_size(b)                                                      \
    (ngx_buf_in_memory(b) ? (off_t) (b->last - b->pos):                      \
                            (b->file_last - b->file_pos))

/*
 * 创建一个ngx_buf_t结构体,并为这个结构体分配size个字节
 * size不包含ngx_buf_t这个结构体的大小
 *
 * 此时buf和chain还没有关联
 */
ngx_buf_t *ngx_create_temp_buf(ngx_pool_t *pool, size_t size);

/*
 *	批量分配ngx_buf_t和ngx_chain_t结构体,并且为ngx_buf_t分配实际管理的内存
 *
 *	入参bufs中指定了要分配的ngx_buf_t的个数,已经每个ngx_buf_t要管理的内存大小
 */
ngx_chain_t *ngx_create_chain_of_bufs(ngx_pool_t *pool, ngx_bufs_t *bufs);


#define ngx_alloc_buf(pool)  ngx_palloc(pool, sizeof(ngx_buf_t))
// 分配一个ngx_buf_t结构体大小的内存空间,并初始化为零
#define ngx_calloc_buf(pool) ngx_pcalloc(pool, sizeof(ngx_buf_t))

/*
 * 创建一个缓冲区链项(创建的是一个单独的链项,需要跟其他的链项链在一起才能形成链)
 *
 * 此时buf和chain还没有关联
 */
ngx_chain_t *ngx_alloc_chain_link(ngx_pool_t *pool);

/*
 * 将链表释放到pool->chain中
 */
#define ngx_free_chain(pool, cl)                                             \
    cl->next = pool->chain;                                                  \
    pool->chain = cl



ngx_int_t ngx_output_chain(ngx_output_chain_ctx_t *ctx, ngx_chain_t *in);
ngx_int_t ngx_chain_writer(void *ctx, ngx_chain_t *in);

/*
 * 将链表in中的buf链接到*chain表的尾部
 *
 * 此操作并没有发生实际数据拷贝动作,链表in中关联的ngx_buf_t结构体不变,只是不再用原来的ngx_chain_t结构体
 * 所以这里的copy实际上指的是ngx_chain_t结构体的拷贝,因为会重新创建该结构体
 *
 * 比如有两个链:
 * 		ngx_chain_t *chain;
 * 		ngx_chain_t *in;
 * 假设这连个链表都有值,那么拷贝方法应该这样调用:
 * 		ngx_chain_add_copy(pool, &chain, in);
 * 注意第二个参数传递的是chain这个链表的变量地址,而不是变量中的值
 *
 */
ngx_int_t ngx_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain,
    ngx_chain_t *in);

/*
 * 从空闲链表中获取一个链表项,如果没有空闲链表则新创建一个链表项
 *
 * 返回的链表项包括ngx_buf_t结构体,但不包括实际的缓存空间
 *
 * 假设有一个空闲链:
 * 		ngx_chain_t *free;
 * 那么该方法应该这样调用
 * 		ngx_chain_get_free_buf(p, &free);
 */
ngx_chain_t *ngx_chain_get_free_buf(ngx_pool_t *p, ngx_chain_t **free);

/*
 * 将链表busy和out中的链表项释放到free链表中,释放的前提是busy和out中的链表项对应的buf缓存空间为零(pos-last 或者 file_pos-file_last为零)
 *
 * 在释放的过程中会首先将out中的链表项追加到busy的尾部,如果busy链表为空则直接释放out链表
 *
 * 如果该方法传入的tag值和busy链表中buf的tag值不同,那么该buf对应的链表项不会释放到free链表中,而是
 * 释放到p->chain链表中
 *
 * 如果在释放的过程中遇到一个buf非空的链表项,则终止后续的释放,在此之前的释放仍有效
 *
 * 所以该方法中的update单词对应那个链表自己体会吧
 */
void ngx_chain_update_chains(ngx_pool_t *p, ngx_chain_t **free,
    ngx_chain_t **busy, ngx_chain_t **out, ngx_buf_tag_t tag);

off_t ngx_chain_coalesce_file(ngx_chain_t **in, off_t limit);

/*
 * 该方法的功能是把链表in中已经发送出去的数据排除掉
 */
ngx_chain_t *ngx_chain_update_sent(ngx_chain_t *in, off_t sent);

#endif /* _NGX_BUF_H_INCLUDED_ */
