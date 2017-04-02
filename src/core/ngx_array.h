
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


/**
 * 涉及自动扩容和内存拷贝，效率不高
 *
 * 小数据量可以考虑,大数据量不建议
 *
 */


#ifndef _NGX_ARRAY_H_INCLUDED_
#define _NGX_ARRAY_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef struct {
	// 数组元素的起始地址
    void        *elts;
    // 已经存储了多少个元素
    ngx_uint_t   nelts;
    // 每个元素所占字节大小
    size_t       size;
    // 数组大小
    ngx_uint_t   nalloc;
    ngx_pool_t  *pool;
} ngx_array_t;


//创建一个ngx_array_t对象
ngx_array_t *ngx_array_create(ngx_pool_t *p, ngx_uint_t n, size_t size);
//尝试将数组a所占用内存,还给pool
void ngx_array_destroy(ngx_array_t *a);
void *ngx_array_push(ngx_array_t *a);
void *ngx_array_push_n(ngx_array_t *a, ngx_uint_t n);


/**
 *初始化一个ngx_array_t对象
 *
 *array: 要初始化的数组
 *pool:用于分配内存的内存池
 *n: 可以存储的元素个数(只是个初始值,可以动态扩容)
 *size: 数组中元素的大小
 */
static ngx_inline ngx_int_t
ngx_array_init(ngx_array_t *array, ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
    /*
     * set "array->nelts" before "array->elts", otherwise MSVC thinks
     * that "array->nelts" may be used without having been initialized
     */

    array->nelts = 0;
    array->size = size;
    array->nalloc = n;
    array->pool = pool;

    //为数组分配n*size个字节
    array->elts = ngx_palloc(pool, n * size);
    if (array->elts == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


#endif /* _NGX_ARRAY_H_INCLUDED_ */

