
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static void *ngx_palloc_block(ngx_pool_t *pool, size_t size);
static void *ngx_palloc_large(ngx_pool_t *pool, size_t size);


/**
 * 创建一个ngx_pool_t对象
 *
 * size: 初始内存池大小
 *       不要小于sizeof(ngx_pool_t),否则会使用未分配的内存 TODO
 * log: 日志对象
 */
ngx_pool_t *
ngx_create_pool(size_t size, ngx_log_t *log)
{
    ngx_pool_t  *p;

    //分配size大小的内存，并按照NGX_POOL_ALIGNMENT字节对齐内存地址
    p = ngx_memalign(NGX_POOL_ALIGNMENT, size, log);
    if (p == NULL) {
        return NULL;
    }

    //指向pool_data内存块中未被分配出去内存的首地址
    p->d.last = (u_char *) p + sizeof(ngx_pool_t);
    //指向pool_data内存块最后地址+1的位置
    p->d.end = (u_char *) p + size;
    //因为是刚分配,所以指向NULL
    p->d.next = NULL;
    p->d.failed = 0;

    //从这里可以看到,如果size小于sizeof(ngx_pool_t)会出现负值,也就是说创建的pool是无效的
    size = size - sizeof(ngx_pool_t);
    //max表示当前poo_data最多可用的字节数
    p->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL;

    //当前正在使用的pool
    p->current = p;
    p->chain = NULL;
    p->large = NULL;
    p->cleanup = NULL;
    p->log = log;

    return p;
}


/**
 * 销毁 *pool
 *
 */
void
ngx_destroy_pool(ngx_pool_t *pool)
{
    ngx_pool_t          *p, *n;
    ngx_pool_large_t    *l;
    ngx_pool_cleanup_t  *c;

    //遍历pool->ngx_pool_cleanup_t并执行其中的ngx_pool_cleanup_pt方法(如果有)
    //用来在销毁整个pool之前做一些收尾工作
    //ngx_pool_cleanup_t是一个链式的结构,用来存放ngx_pool_cleanup_pt方法
    for (c = pool->cleanup; c; c = c->next) {
        if (c->handler) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "run cleanup: %p", c);
            c->handler(c->data);
        }
    }

    //销毁large中的内存
    for (l = pool->large; l; l = l->next) {

        ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0, "free: %p", l->alloc);

        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

#if (NGX_DEBUG)

    /*
     * we could allocate the pool->log from this pool
     * so we cannot use this log while free()ing the pool
     */

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                       "free: %p, unused: %uz", p, p->d.end - p->d.last);

        if (n == NULL) {
            break;
        }
    }

#endif

    //循环销毁pool链中的每个pool
    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        //该函数会调用系统函数free()去销毁内存
        ngx_free(p);

        if (n == NULL) {
            break;
        }
    }
}


/**
 * 重置pool,相当于清空pool中的所有数据
 *
 * *pool: 内存池
 */
void
ngx_reset_pool(ngx_pool_t *pool)
{
    ngx_pool_t        *p;
    ngx_pool_large_t  *l;

    //销毁所有的large
    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

    //重置pool链中所有pool_data的last指针,使其指向初始值
    for (p = pool; p; p = p->d.next) {
        p->d.last = (u_char *) p + sizeof(ngx_pool_t);
        p->d.failed = 0;
    }

    pool->current = pool;
    pool->chain = NULL;
    pool->large = NULL;
}


/**
 * 从*pool中分配size个字节,并对齐其内存地址
 * pool: 内存池
 * size: 分配size个字节
 */
void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    ngx_pool_t  *p;

    //pool的最大可分配内存满足这次要分配的size
    if (size <= pool->max) {

        p = pool->current;

        do {
            //对d.last地址对齐,也就是说 m - p->d.last 的结果会大于等于零,因为地址对齐肯定会大于等于原来的地址
            m = ngx_align_ptr(p->d.last, NGX_ALIGNMENT);

            //如果地址对齐后,在当前pool_data中剩余的内存大小,大于等于这次要分配的内存size,
            //则从当前pool_data中直接分配出去
            if ((size_t) (p->d.end - m) >= size) {
                //最后未分配内存的首地址后移 m+size 个字节
                p->d.last = m + size;

                //printf("从pool链中分配; pool地址是:%p; pool->current地址是:%p \n",pool,pool->current);

                return m;
            }

            //走到这一步说明当前pool_data中剩余的内存不够这次分配的
            //试着从下一个pool中查找可用的内存
            p = p->d.next;

        } while (p);

        //走到这一步说明当前pool链中,没有一个poool_data可以满足size大小的内存
        //下面这个方法会在pool链中新增一个pool对象,以满足这次对size大小字节的分配
        return ngx_palloc_block(pool, size);
    }

    //走到这一步说明ngx_pool_data_t中的容量,无法满足这次要分配的字节个数size
    //下面这个方法用来分配,超过ngx_pool_data_t容量的内存
    //
    return ngx_palloc_large(pool, size);
}


/**
 * 从*pool中分配size大小的字节,但不会对齐内存地址
 *
 * pool: 内存池
 * size: 分配字节大小
 */
void *
ngx_pnalloc(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    ngx_pool_t  *p;

    if (size <= pool->max) {

        p = pool->current;

        do {
            m = p->d.last;

            if ((size_t) (p->d.end - m) >= size) {
                p->d.last = m + size;

                return m;
            }

            p = p->d.next;

        } while (p);

        return ngx_palloc_block(pool, size);
    }

    return ngx_palloc_large(pool, size);
}


/**
 * 新创建一个new_pool,并将其放入*pool链的末端
 * 从new_pool中分配size大小的字节
 *
 * pool: 内存池
 * size: 内存大小
 */
static void *
ngx_palloc_block(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    size_t       psize;
    ngx_pool_t  *p, *new;

    //其实就是最初调用ngx_create_pool方法时使用的size
    //这一步会保证链中的所有pool分配的内存大小都一致
    psize = (size_t) (pool->d.end - (u_char *) pool);

    //分配psize大小的内存，并按照NGX_POOL_ALIGNMENT字节对齐内存地址
    m = ngx_memalign(NGX_POOL_ALIGNMENT, psize, pool->log);
    if (m == NULL) {
        return NULL;
    }

    new = (ngx_pool_t *) m;

    //m是对齐后的pool的开始地址,加上psize就是当前pool_data的边界值
    new->d.end = m + psize;
    new->d.next = NULL;
    new->d.failed = 0;

    //这一步相当于计算new->d.last的初始值，在ngx_create_pool()方法中d.last
    //的值是 p + sizeof(ngx_pool_t),会留出整个pool对象的大小;
    //但是在这里只留出了ngx_pool_data_t的大小,所以new中的max、*current等占用的内存,都会被覆盖掉
    //从这里可以看到pool链中,只有刚开始创建时pool对象才会在内存中保存完整的结构,其它的pool都只保留
    //ngx_pool_data_t部分。这也就是为什么pool用来表示链关系的next放到了ngx_pool_data_t中。
    m += sizeof(ngx_pool_data_t);
    m = ngx_align_ptr(m, NGX_ALIGNMENT);
    new->d.last = m + size;

    //循环pool链中的所有pool,如果循环到的pool做为current角色时,分配内存时失败的次数大于4,
    //则替换掉其作为current的资格
    for (p = pool->current; p->d.next; p = p->d.next) {
        if (p->d.failed++ > 4) {
            pool->current = p->d.next;
        }
    }

    //将新创建的pool对象放入到pool链的末端
    p->d.next = new;

    return m;
}


/**
 * 用来分配超过pool->ngx_pool_data_t容量的内存
 *
 * 新分配的内存使用ngx_pool_large_t对象引用,并且该对象会放入pool->large链表顶部
 *
 * pool: 内存池
 * size: 内存大小
 *
 */
static void *
ngx_palloc_large(ngx_pool_t *pool, size_t size)
{
    void              *p;
    ngx_uint_t         n;
    ngx_pool_large_t  *large;

    //分配size大小的内存(malloc)
    p = ngx_alloc(size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    n = 0;
    //从pool->large链表的前四个对象中找是否有空置的large对象
    //如果有则不在创建该数据结构,直接使用原来的结构
    for (large = pool->large; large; large = large->next) {
        if (large->alloc == NULL) {
            large->alloc = p;
            return p;
        }

        if (n++ > 3) {
            break;
        }
    }

    //创建一个ngx_pool_large_t,并将其放入到pool->large链表的顶部
    large = ngx_palloc(pool, sizeof(ngx_pool_large_t));
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    //分配的内存赋值给large->alloc
    large->alloc = p;
    //将刚创建的large放入pool->large链表顶部
    large->next = pool->large;
    pool->large = large;

    //返回分配的内存地址
    return p;
}


/**
 *该方法类似ngx_palloc_large方法的一个内存对齐版本
 *
 *分配一个size字节的对齐内存,该内存按照alignment字节对内存对齐,比如按512字节对齐(磁盘块大小)
 *
 *新分配的内存放到ngx_pool_large_t对象中,该对象最终会被放入到pool->large的顶部
 */
void *
ngx_pmemalign(ngx_pool_t *pool, size_t size, size_t alignment)
{
    void              *p;
    ngx_pool_large_t  *large;

    //分配size个字节,并按照alignment字节大小对齐地址
    p = ngx_memalign(alignment, size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    large = ngx_palloc(pool, sizeof(ngx_pool_large_t));
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}


/**
 * 释放存放在pool->large链表中的*p内存
 * 实际上*p指向的内存应该是有ngx_pmemalign方法分配而来
 */
ngx_int_t
ngx_pfree(ngx_pool_t *pool, void *p)
{
    ngx_pool_large_t  *l;

    for (l = pool->large; l; l = l->next) {
        if (p == l->alloc) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "free: %p", l->alloc);
            ngx_free(l->alloc);
            l->alloc = NULL;

            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


/**
 * 从*pool中分配size大小的字节,对齐地址并对内存初始化
 */
void *
ngx_pcalloc(ngx_pool_t *pool, size_t size)
{
    void *p;

    p = ngx_palloc(pool, size);
    if (p) {
        ngx_memzero(p, size);
    }

    return p;
}


/**
 * 向pool->cleanup链末尾添加一个ngx_pool_cleanup_t对象
 * 其中size是为ngx_pool_cleanup_t->data分配的内存大小
 */
ngx_pool_cleanup_t *
ngx_pool_cleanup_add(ngx_pool_t *p, size_t size)
{
    ngx_pool_cleanup_t  *c;

    c = ngx_palloc(p, sizeof(ngx_pool_cleanup_t));
    if (c == NULL) {
        return NULL;
    }

    if (size) {
        c->data = ngx_palloc(p, size);
        if (c->data == NULL) {
            return NULL;
        }

    } else {
        c->data = NULL;
    }

    c->handler = NULL;
    c->next = p->cleanup;

    p->cleanup = c;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, p->log, 0, "add cleanup: %p", c);

    return c;
}


/**
 * 该方法用来关闭文件描述符fd
 * 该方法会遍历出所有pool->cleanup(ngx_pool_cleanup_t)链中handler是ngx_pool_cleanup_file方法
 * 的对象，然后执行该方法。
 *
 * 注意：
 *   如果pool->cleanup链中某一个ngx_pool_cleanup_t对象,其handler等于ngx_pool_cleanup_file方法，
 *   那么该对象中的*data指向的一定是ngx_pool_cleanup_file_t对象
 *
 */
void
ngx_pool_run_cleanup_file(ngx_pool_t *p, ngx_fd_t fd)
{
    ngx_pool_cleanup_t       *c;
    ngx_pool_cleanup_file_t  *cf;

    for (c = p->cleanup; c; c = c->next) {
        if (c->handler == ngx_pool_cleanup_file) {

            cf = c->data;

            if (cf->fd == fd) {
                c->handler(cf);
                c->handler = NULL;
                return;
            }
        }
    }
}


/**
 * 关闭*data结构(ngx_pool_cleanup_file_t)中的fd
 */
void
ngx_pool_cleanup_file(void *data)
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d",
                   c->fd);

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}


/**
 * 删除*data结构(ngx_pool_cleanup_file_t)中名字为*name的文件,然后在关闭该结构中的fd
 */
void
ngx_pool_delete_file(void *data)
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_err_t  err;

    ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d %s",
                   c->fd, c->name);

    if (ngx_delete_file(c->name) == NGX_FILE_ERROR) {
        err = ngx_errno;

        if (err != NGX_ENOENT) {
            ngx_log_error(NGX_LOG_CRIT, c->log, err,
                          ngx_delete_file_n " \"%s\" failed", c->name);
        }
    }

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}


#if 0

static void *
ngx_get_cached_block(size_t size)
{
    void                     *p;
    ngx_cached_block_slot_t  *slot;

    if (ngx_cycle->cache == NULL) {
        return NULL;
    }

    slot = &ngx_cycle->cache[(size + ngx_pagesize - 1) / ngx_pagesize];

    slot->tries++;

    if (slot->number) {
        p = slot->block;
        slot->block = slot->block->next;
        slot->number--;
        return p;
    }

    return NULL;
}

#endif
