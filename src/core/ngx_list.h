
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/**
 * 一个数组加链表的容器
 *
 * 其中ngx_list_part_s代表容器中的链表项(列表项),每个链表项会持有一个块
 * 连续的内存,最终的元素数据就放在该列表项中。
 *
 * 另外ngx_list_part_s还负责指向下一个链表项。
 *
 * ngx_list_t就代表容器本身,有两个字段分别指向链表的头和尾部。
 *
 */


#ifndef _NGX_LIST_H_INCLUDED_
#define _NGX_LIST_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef struct ngx_list_part_s  ngx_list_part_t;

//用来存放元素的结构体(称为链表项)
struct ngx_list_part_s {
	// 元素起始地址
    void             *elts;
    // 当前结构体中已经存放了nelts个元素(可以理解为已存储元素计数器)
    ngx_uint_t        nelts;
    // 指向链表中下一个ngx_list_part_t
    ngx_list_part_t  *next;
};


// 实际上ngx_list_t是一个数组加链表的结合体
// 每个链表项自身是一个数组,并且每个链表项又可以指向下一个链表项
typedef struct {
	// 指向链表中的最后一个链表项
    ngx_list_part_t  *last;
    // 链表中的第一个链表项
    ngx_list_part_t   part;
    // 链表中每个元素所占字节个数
    size_t            size;
    // 每个链表项可以容纳的元素个数
    ngx_uint_t        nalloc;
    ngx_pool_t       *pool;
} ngx_list_t;


ngx_list_t *ngx_list_create(ngx_pool_t *pool, ngx_uint_t n, size_t size);


/**
 * 初始化链表
 * *list: 指向ngx_list_t的指针地址
 * *pool: 用来分配内存的pool
 * n: 每个链表项可容纳的元素个数
 * size: 链表中每个元素所占字节个数
 */
static ngx_inline ngx_int_t
ngx_list_init(ngx_list_t *list, ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
	// 为链表项part分配用于真正存放数据的内存
    list->part.elts = ngx_palloc(pool, n * size);
    if (list->part.elts == NULL) {
        return NGX_ERROR;
    }

    list->part.nelts = 0; // 表头已存储的元素个数初始化为0
    list->part.next = NULL;
    list->last = &list->part; // last指向表头,因为是初始化，只有一个链表项
    list->size = size; // 初始化元素大小
    list->nalloc = n; // 初始化链表项可存放的元素个数
    list->pool = pool;

    return NGX_OK;
}


/*
 *
 *  the iteration through the list:
 *
 *  part = &list.part;
 *  data = part->elts;
 *
 *  for (i = 0 ;; i++) {
 *
 *      if (i >= part->nelts) {
 *          if (part->next == NULL) {
 *              break;
 *          }
 *
 *          part = part->next;
 *          data = part->elts;
 *          i = 0;
 *      }
 *
 *      ...  data[i] ...
 *
 *  }
 */


void *ngx_list_push(ngx_list_t *list);


#endif /* _NGX_LIST_H_INCLUDED_ */

