
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * ------------------------变量初始化基本步骤-----------------------
 * 1.在解析到http指令后执行ngx_http_block()方法,该方法通过回调ngx_http_core_preconfiguration()方法
 *   来执行ngx_http_variables_add_core_vars()方法
 *
 * 2.ngx_http_variables_add_core_vars()方法会把ngx_http_variables.c/ngx_http_core_variables中
 *   定义的内置变量(http_host、http_cookie等)放入到cmcf->variables_keys变量中
 *
 * 3.等http配置块内的所有指令都解析完毕后开始执行ngx_http_variables_init_vars()方法,该方法用来为所有的
 *   变量设置get_handler()方法,包括第三方模块定义的内部变量和set定义的变量,也包括动态变量(比如以http_开头的)
 *
 *
 *
 *
 * ngx_http_add_variable()方法和ngx_http_get_variable_index()方法可以理解为是创建变量的
 * 比如定义变量name:
 *    set name 张三
 * 上面两个方法调用完毕后,还需要设置变量的get_handler()方法,因为handler()方法才是获取变量值的方法
 *
 *
 *
 */

#ifndef _NGX_HTTP_VARIABLES_H_INCLUDED_
#define _NGX_HTTP_VARIABLES_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


/*
 * 变量的值
 */
typedef ngx_variable_value_t  ngx_http_variable_value_t;

#define ngx_http_variable(v)     { sizeof(v) - 1, 1, 0, 0, 0, (u_char *) v }

/*
 * 变量的名字
 */
typedef struct ngx_http_variable_s  ngx_http_variable_t;

typedef void (*ngx_http_set_variable_pt) (ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
typedef ngx_int_t (*ngx_http_get_variable_pt) (ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);


#define NGX_HTTP_VAR_CHANGEABLE   1
#define NGX_HTTP_VAR_NOCACHEABLE  2
#define NGX_HTTP_VAR_INDEXED      4
#define NGX_HTTP_VAR_NOHASH       8


/*
 * 表示一个变量名
 *
 * ngx中的变量基本会存在两个地方,一个是hash结构中,另一个是数组
 *
 * ngx_http_variable_value_t表示一个变量值
 */
struct ngx_http_variable_s {
    /*
     * 对应变量名字
     */
    ngx_str_t                     name;   /* must be first to build the hash */

    /*
     * 目前使用这个方法的地方:
     *  /http/modules/ngx_http_auth_request_module.c:279
     */
    ngx_http_set_variable_pt      set_handler;
    ngx_http_get_variable_pt      get_handler;

    /*
     * get_handler回调方法时传递的参数,如何使用这个data则由实现get_handler方法的业务定义,目前由如下使用方式:
     *
     * 存放具体变量值的偏移量,比如指向ngx_http_request_t结构体的args字段.
     * 如果没用则为0,比如变量值不是简单的指向ngx_http_request_t结构体的某个字段,而是需要复杂的分析计算,那
     * 么就需要使用get_handler方法中的特殊逻辑了
     */
    uintptr_t                     data;

    /*
     * NGX_HTTP_VAR_CHANGEABLE:  该变量可重复设置,也就是说该变量值可变
     * NGX_HTTP_VAR_NOCACHEABLE: 该变量不可缓存(每次都调用get_handler())
     * NGX_HTTP_VAR_INDEXED: 该变量可以用索引来读取
     * NGX_HTTP_VAR_NOHASH:  该变量没有hash到hash结构中 ?
     */
    ngx_uint_t                    flags;

    /*
     * 该变量在cmcf->variables内的下标
     * 可以通过ngx_http_get_variable_index()方法获取
     */
    ngx_uint_t                    index;
};


ngx_http_variable_t *ngx_http_add_variable(ngx_conf_t *cf, ngx_str_t *name,
    ngx_uint_t flags);
ngx_int_t ngx_http_get_variable_index(ngx_conf_t *cf, ngx_str_t *name);
ngx_http_variable_value_t *ngx_http_get_indexed_variable(ngx_http_request_t *r,
    ngx_uint_t index);
ngx_http_variable_value_t *ngx_http_get_flushed_variable(ngx_http_request_t *r,
    ngx_uint_t index);

ngx_http_variable_value_t *ngx_http_get_variable(ngx_http_request_t *r,
    ngx_str_t *name, ngx_uint_t key);

ngx_int_t ngx_http_variable_unknown_header(ngx_http_variable_value_t *v,
    ngx_str_t *var, ngx_list_part_t *part, size_t prefix);


#if (NGX_PCRE)

typedef struct {
    ngx_uint_t                    capture;
    ngx_int_t                     index;
} ngx_http_regex_variable_t;


typedef struct {
    ngx_regex_t                  *regex;
    ngx_uint_t                    ncaptures;
    ngx_http_regex_variable_t    *variables;
    ngx_uint_t                    nvariables;
    ngx_str_t                     name;
} ngx_http_regex_t;


typedef struct {
    ngx_http_regex_t             *regex;
    void                         *value;
} ngx_http_map_regex_t;


ngx_http_regex_t *ngx_http_regex_compile(ngx_conf_t *cf,
    ngx_regex_compile_t *rc);
ngx_int_t ngx_http_regex_exec(ngx_http_request_t *r, ngx_http_regex_t *re,
    ngx_str_t *s);

#endif


typedef struct {
    ngx_hash_combined_t           hash;
#if (NGX_PCRE)
    ngx_http_map_regex_t         *regex;
    ngx_uint_t                    nregex;
#endif
} ngx_http_map_t;


void *ngx_http_map_find(ngx_http_request_t *r, ngx_http_map_t *map,
    ngx_str_t *match);


ngx_int_t ngx_http_variables_add_core_vars(ngx_conf_t *cf);
ngx_int_t ngx_http_variables_init_vars(ngx_conf_t *cf);


extern ngx_http_variable_value_t  ngx_http_variable_null_value;
extern ngx_http_variable_value_t  ngx_http_variable_true_value;


#endif /* _NGX_HTTP_VARIABLES_H_INCLUDED_ */
