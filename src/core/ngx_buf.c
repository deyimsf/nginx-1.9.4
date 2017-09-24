
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * 把实际缓存和链分开,这种设计对内存的管理更灵活
 * 1.职责划分更清晰,缓存就是缓存，链表就是链表
 * 2.当需要使用小块内存是可以直接选取小的ngx_buf_t,避免直接使用大ngx_buf_t造成内存的浪费
 *
 */


#include <ngx_config.h>
#include <ngx_core.h>


/*
 * 创建一个ngx_buf_t结构体,并为这个结构体分配size个字节
 * size不包含ngx_buf_t这个结构体的大小
 *
 * 此时buf和chain还没有关联
 */
ngx_buf_t *
ngx_create_temp_buf(ngx_pool_t *pool, size_t size)
{
    ngx_buf_t *b;

    // 分配一个ngx_buf_t结构体占用的内存
    b = ngx_calloc_buf(pool);
    if (b == NULL) {
        return NULL;
    }

    // 为ngx_buf_t分配缓存空间
    b->start = ngx_palloc(pool, size);
    if (b->start == NULL) {
        return NULL;
    }

    /*
     * set by ngx_calloc_buf():
     *
     *     b->file_pos = 0;
     *     b->file_last = 0;
     *     b->file = NULL;
     *     b->shadow = NULL;
     *     b->tag = 0;
     *     and flags
     */

    // 初始化
    b->pos = b->start;
    b->last = b->start;
    b->end = b->last + size;
    // 该缓存时一个临时缓存,缓存数据可以改变
    b->temporary = 1;

    return b;
}


/*
 * 创建一个缓冲区链项(创建的是一个单独的链项,需要跟其他的链项链在一起才能形成链)
 *
 * 此时buf和chain还没有关联
 */
ngx_chain_t *
ngx_alloc_chain_link(ngx_pool_t *pool)
{
    ngx_chain_t  *cl;

    /*
     * 优先从可用链表中获取该链项,如果没有则再从pool中分配
     */
    cl = pool->chain;

    if (cl) {
    	/*
    	 * chain中存在可用的链项,则将链头指向该链头的下一个链项,然后返回老链头
    	 */

        pool->chain = cl->next;
        return cl;
    }

    /*
     * pool中不存在可用的链项,则直接从pool中为链项分配内存
     */
    cl = ngx_palloc(pool, sizeof(ngx_chain_t));
    if (cl == NULL) {
        return NULL;
    }

    return cl;
}


/*
 *	批量分配ngx_buf_t和ngx_chain_t结构体,并且为ngx_buf_t分配实际管理的内存
 *
 *	入参bufs中指定了要分配的ngx_buf_t的个数,已经每个ngx_buf_t要管理的内存大小
 *
 */
ngx_chain_t *
ngx_create_chain_of_bufs(ngx_pool_t *pool, ngx_bufs_t *bufs)
{
    u_char       *p;
    ngx_int_t     i;
    ngx_buf_t    *b;
    ngx_chain_t  *chain, *cl, **ll;

    /*
     * 创建所有bufs->num个结构体所要管理的内存
     * 此时分配的内存不包括ngx_buf_t结构体本身
     */
    p = ngx_palloc(pool, bufs->num * bufs->size);
    if (p == NULL) {
        return NULL;
    }

    /*
     * 这两个变量内存结构如下:
     *     ll			&chain
     *    -----			-----
     *    | * |			| * |
     *    -----			-----
     *    	   \ chain /
     *			-------
     *			|  *  |
     *			-------
     *	ll保存的是chain这个指针变量的地址,并不是chain中的指针值
     */
    ll = &chain;

    for (i = 0; i < bufs->num; i++) {

    	/*
    	 * 创建一个ngx_buf_t结构体,或者说为这个结构体分配内存空间
    	 */
        b = ngx_calloc_buf(pool);
        if (b == NULL) {
            return NULL;
        }

        /*
         * set by ngx_calloc_buf():
         *
         *     b->file_pos = 0;
         *     b->file_last = 0;
         *     b->file = NULL;
         *     b->shadow = NULL;
         *     b->tag = 0;
         *     and flags
         *
         */

        /*
         * 刚开始的时候pos和last是相等的,都赋值p
         * p代表一块被ngx_buf_t实际管理空间的开始位置
         */
        b->pos = p;
        b->last = p;
        // buf中的数据可改变
        b->temporary = 1;

        /*
         * 用start和end为ngx_buf_t要管理的空间定界
         */
        b->start = p;
        // p加上每个buf要管理的实际空间大小,这样p就是本buf要管理空间的结尾地址,同时也是下一个buf管理空间的开始地址
        p += bufs->size;
        b->end = p;

        /*
         * 为这个ngx_buf_t分配一个ngx_chain_t
         */
        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NULL;
        }

        /*
         * 将新创建的链项放入到链尾部
         *
         * ll变量存放的是链尾项的next指针变量的地址,所以*ll变量和next是等价的,内存结构体如下:
         *     ll		    &cl->next
         *    -----			  -----
         *    | * |			  | * |
         *    -----			  -----
         *    	   \ cl->next /
         *			----------
         *			|    *   |
         *			----------
         * 有上图可以知道
         * 		*ll = cl;
         * 这一句实际上就是把新建的链表项,追加到链表尾部
         */
        cl->buf = b;
        *ll = cl;
        ll = &cl->next;
    }

    // 尾部链表项的next设置为
    *ll = NULL;

    // 返回创建好的链表的头
    return chain;
}


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
ngx_int_t
ngx_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain, ngx_chain_t *in)
{
    ngx_chain_t  *cl, **ll;

    /*
     *		ll			chain
     *	   -----		-----
     *	   | * |        | * |
     *	   -----        -----
     *	   		\      /
     *	   		 -----
     *	   		 | * |
     *	   		 -----
     *	   		   |
     *	   	---------------
     *	   	| ngx_chain_t |
     *	   	---------------
     */
    ll = chain;

    /*
     * 该循环是找出链表的尾部
     * 最终,ll存放的是尾部链表变量(cl->next)的地址(&cl->next)
     * *ll就是存放尾部链表项的变量
     */
    for (cl = *chain; cl; cl = cl->next) {
        ll = &cl->next;
    }

    /*
     * 开始所谓的copy,实际上只是copy出ngx_chain_t结构体
     */
    while (in) {
        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        // 老链表项的buf赋值给新链表项
        cl->buf = in->buf;
        /*
         * 将新创建的链表项cl放到老链表的尾部
         * *ll就是存放尾部链表项的变量
         */
        *ll = cl;
        /*
         * 将新创建的cl的next变量的地址赋值给ll
         * 如此一来ll变量就始终保存着尾部链表项变量的地址
         */
        ll = &cl->next;
        in = in->next;
    }

    *ll = NULL;

    return NGX_OK;
}


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
ngx_chain_t *
ngx_chain_get_free_buf(ngx_pool_t *p, ngx_chain_t **free)
{
    ngx_chain_t  *cl;

    /*
     * 检查*free链表中是否存在链表项,存在则返回该链表项
     */
    if (*free) {
    	// 取出链表头
        cl = *free;
        // 将链表头的下一个链表项设置为链表头
        *free = cl->next;
        // 之前取出的链表头的next设置为NULL
        cl->next = NULL;
        return cl;
    }

    /*
     * 走到这里说明*free不存在,所以需要重新分配一个ngx_chain_t
     */
    cl = ngx_alloc_chain_link(p);
    if (cl == NULL) {
        return NULL;
    }

    // 重新分配一个ngx_buf_t结构体
    cl->buf = ngx_calloc_buf(p);
    if (cl->buf == NULL) {
        return NULL;
    }

    // 初始化该cl的next字段
    cl->next = NULL;

    return cl;
}


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
void
ngx_chain_update_chains(ngx_pool_t *p, ngx_chain_t **free, ngx_chain_t **busy,
    ngx_chain_t **out, ngx_buf_tag_t tag)
{
    ngx_chain_t  *cl;

    if (*busy == NULL) {
    	/*
    	 * 传入的*busy链表为空,则直接释放*out链表
    	 */

        *busy = *out;

    } else {
    	/*
    	 * 将链表*out追加到*busy链表中
    	 */
        for (cl = *busy; cl->next; cl = cl->next) { /* void */ }

        cl->next = *out;
    }

    *out = NULL;

    while (*busy) {
        cl = *busy;

        // 如果该链表项对应的buf中还有数据说明无法释放,则直接break
        if (ngx_buf_size(cl->buf) != 0) {
            break;
        }

        if (cl->buf->tag != tag) {
            *busy = cl->next;
            // free到p->chain中
            ngx_free_chain(p, cl);
            continue;
        }

        // 重置buf->pos和buf->last
        cl->buf->pos = cl->buf->start;
        cl->buf->last = cl->buf->start;

        /*
         * cl是从*busy链表中取下的链头
         *
         * *busy链头设置为cl的下一个链表项
         *
         * 将cl设置为链表*free的链头
         */
        *busy = cl->next;
        cl->next = *free;
        *free = cl;
    }
}


off_t
ngx_chain_coalesce_file(ngx_chain_t **in, off_t limit)
{
    off_t         total, size, aligned, fprev;
    ngx_fd_t      fd;
    ngx_chain_t  *cl;

    total = 0;

    cl = *in;
    fd = cl->buf->file->fd;

    do {
        size = cl->buf->file_last - cl->buf->file_pos;

        if (size > limit - total) {
            size = limit - total;

            aligned = (cl->buf->file_pos + size + ngx_pagesize - 1)
                       & ~((off_t) ngx_pagesize - 1);

            if (aligned <= cl->buf->file_last) {
                size = aligned - cl->buf->file_pos;
            }
        }

        total += size;
        fprev = cl->buf->file_pos + size;
        cl = cl->next;

    } while (cl
             && cl->buf->in_file
             && total < limit
             && fd == cl->buf->file->fd
             && fprev == cl->buf->file_pos);

    *in = cl;

    return total;
}


ngx_chain_t *
ngx_chain_update_sent(ngx_chain_t *in, off_t sent)
{
    off_t  size;

    for ( /* void */ ; in; in = in->next) {

        if (ngx_buf_special(in->buf)) {
            continue;
        }

        if (sent == 0) {
            break;
        }

        size = ngx_buf_size(in->buf);

        if (sent >= size) {
            sent -= size;

            if (ngx_buf_in_memory(in->buf)) {
                in->buf->pos = in->buf->last;
            }

            if (in->buf->in_file) {
                in->buf->file_pos = in->buf->file_last;
            }

            continue;
        }

        if (ngx_buf_in_memory(in->buf)) {
            in->buf->pos += (size_t) sent;
        }

        if (in->buf->in_file) {
            in->buf->file_pos += sent;
        }

        break;
    }

    return in;
}
