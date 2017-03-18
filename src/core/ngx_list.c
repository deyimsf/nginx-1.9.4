
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


/**
 * 创建一个ngx_list_t对象,失败则放回NULL
 * *pool: 用来分配内存的pool
 * n: 每个链表项可容纳的元素个数
 * size: 链表中每个元素所占字节个数
 */
ngx_list_t *
ngx_list_create(ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
    ngx_list_t  *list;

    list = ngx_palloc(pool, sizeof(ngx_list_t));
    if (list == NULL) {
        return NULL;
    }

    if (ngx_list_init(list, pool, n, size) != NGX_OK) {
        return NULL;
    }

    return list;
}


/**
 * 向链表中增加一个元素
 * 该方法并没有一个真正的元素入参,只是返回一个可用的元素地址,
 * 用户将数据放入到该地址就算向链表中放入了数据
 *
 *  *l: 链表容器
 */
void *
ngx_list_push(ngx_list_t *l)
{
    void             *elt;
    ngx_list_part_t  *last;

    // 从last中返回可用的内存,因为前面的已经被分配完了
    last = l->last;

    // 最后一个链表项也满了,就新建一个链表项
    if (last->nelts == l->nalloc) {

        /* the last part is full, allocate a new list part */

        last = ngx_palloc(l->pool, sizeof(ngx_list_part_t));
        if (last == NULL) {
            return NULL;
        }

        // 为新建的链表项分配内存
        last->elts = ngx_palloc(l->pool, l->nalloc * l->size);
        if (last->elts == NULL) {
            return NULL;
        }

        last->nelts = 0;
        last->next = NULL;

        // 链表的last指向新建的链表项
        l->last->next = last;
        l->last = last;
    }

    // 从链表项中分配一个元素的内存大小
    elt = (char *) last->elts + l->size * last->nelts;
    // 链表项中已存储元素计数器加一
    last->nelts++;

    return elt;
}
