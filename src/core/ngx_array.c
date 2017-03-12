
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


/**
 * 创建一个ngx_array_t对象
 * p: 内存池
 * n: 可以存储的元素个数
 * size: 每个元素的大小
 */
ngx_array_t *
ngx_array_create(ngx_pool_t *p, ngx_uint_t n, size_t size)
{
    ngx_array_t *a;

    //从pool中分配一个ngx_array_t对象
    a = ngx_palloc(p, sizeof(ngx_array_t));
    if (a == NULL) {
        return NULL;
    }

    //初始化数组
    if (ngx_array_init(a, p, n, size) != NGX_OK) {
        return NULL;
    }

    return a;
}


/**
 * 尝试将数组a所占用内存,还给pool
 *
 * 销毁数组时并没有将内存释放，而是将内存还给了内存池(pool)
 *
 * 使用这个方法销毁数组时条件比较苛刻:
 * 1.在使用ngx_array_create方法创建a时,分配的内存必须是有pool链表中第一个pool来分配
 * 2.在分配完内存后,其它用pool来分配内存的方法,并没有使用pool链表中的第一个pool
 * 如果上面两个条件不满足,则无法释放内存.
 *
 * 更好一点的方法是遍历pool链表和pool->large链表来释放内存。
 */
void
ngx_array_destroy(ngx_array_t *a)
{
    ngx_pool_t  *p;

    p = a->pool;

    //数组存储元素的起始位置 + 元素大小 * 当前数组可容纳的最大数组个数，这三个数计算的结果
    //和当前内存池中d.last的地址做比较
    //如果相等则代表,池p在调用完ngx_array_create方法生成当前数组a后,可能没有做任何的内存分配
    if ((u_char *) a->elts + a->size * a->nalloc == p->d.last) {
        p->d.last -= a->size * a->nalloc;
    }

    //释放ngx_array_t本身所占用的内存
    if ((u_char *) a + sizeof(ngx_array_t) == p->d.last) {
        p->d.last = (u_char *) a;
    }
}


/**
 * 将一个元素放入数组a中
 *
 * 该方法并没有一个待放入元素的入参，而是返回一个地址,用户可以把自己的数据放入到该地址
 *
 * a: 数组
 */
void *
ngx_array_push(ngx_array_t *a)
{
    void        *elt, *new;
    size_t       size;
    ngx_pool_t  *p;

    // 如果数组中已经存储的元素个数,已经到达了数组容量的上限
    // 对数组进行扩容
    if (a->nelts == a->nalloc) {

        /* the array is full */

        size = a->size * a->nalloc; //整个数组占用内存的大小

        p = a->pool;

        if ((u_char *) a->elts + size == p->d.last
            && p->d.last + a->size <= p->d.end)
        {

        	/**
        	 *	走到这里，说明数组a是用内存池中最顶端的pool->d分配的内存
        	 *	并且pool->d中仍然有足够的空间可以分配一个元素的大小
        	 */

            /*
             * the array allocation is the last in the pool
             * and there is space for new allocation
             */

            p->d.last += a->size;
            //扩容一个元素
            a->nalloc++;

        } else {
        	//扩容数组

            /* allocate a new array */

        	//按照原始内存两倍大小扩容; size是字节数
            new = ngx_palloc(p, 2 * size);
            if (new == NULL) {
                return NULL;
            }

            // 将原数组a的数据拷贝到新数组new中; size是字节数
            ngx_memcpy(new, a->elts, size);
            a->elts = new;
            a->nalloc *= 2;
        }
    }

    //a->nelts < a->nalloc
    //已有元素个数小于数组最大容量
    //直接分配元素地址
    elt = (u_char *) a->elts + a->size * a->nelts;
    //已有元素个数加一
    a->nelts++;

    return elt;
}


/**
 * 将一n个元素放入数组a中
 *
 * 方法ngx_array_push的批量版本
 *
 * a: 数组
 * n: 放入的元素个数
 */
void *
ngx_array_push_n(ngx_array_t *a, ngx_uint_t n)
{
    void        *elt, *new;
    size_t       size;
    ngx_uint_t   nalloc;
    ngx_pool_t  *p;

    size = n * a->size;

    if (a->nelts + n > a->nalloc) {

        /* the array is full */

        p = a->pool;

        if ((u_char *) a->elts + a->size * a->nalloc == p->d.last
            && p->d.last + size <= p->d.end)
        {
            /*
             * the array allocation is the last in the pool
             * and there is space for new allocation
             */

            p->d.last += size;
            a->nalloc += n;

        } else {
            /* allocate a new array */

            nalloc = 2 * ((n >= a->nalloc) ? n : a->nalloc);

            new = ngx_palloc(p, nalloc * a->size);
            if (new == NULL) {
                return NULL;
            }

            ngx_memcpy(new, a->elts, a->nelts * a->size);
            a->elts = new;
            a->nalloc = nalloc;
        }
    }

    elt = (u_char *) a->elts + a->size * a->nelts;
    a->nelts += n;

    return elt;
}
