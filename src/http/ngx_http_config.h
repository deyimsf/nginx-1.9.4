
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_CONFIG_H_INCLUDED_
#define _NGX_HTTP_CONFIG_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


/*
 * 用来存储核心http模块配置信息的结构体。
 *
 * 该结构体算是核心http模块的主配置结构体,但是http核心模块也用到了该结构体来关联各个http模块,但它
 * 不是http核心模块的主配置信息结构体,http核心模块的主配置信息结构体有三个,是ngx_http_core_main|srv|loc_conf_t。
 *
 * 有三个地方用到了该结构体
 * 1.核心http模块(ngx_http_module)在遇到http{}指令后,会创建ngx_http_conf_ctx_t结构体,
 *   所以一个http{}区域对应一个ngx_http_conf_ctx_t结构体。
 *
 *   http指令对应的方法是/src/http/ngx_http.c/ngx_http_block()
 *
 * 	 cycle->conf_ctx
 *   -----
 *	 | * |
 *	 -----
 *	  \            ngx_http_module.index
 *	   -----------------
 *	   | * | * | * | * |
 *     -----------------
 *    				 \
 *    			     -----------------------
 *    			     | ngx_http_conf_ctx_t |
 *			         -----------------------
 *
 *
 * 2.http核心模块(ngx_http_core_module)在遇到server指令后会创建ngx_http_conf_ctx_t结构体,
 *   同http{}区域一样,一个server{}区域也会对应一个ngx_http_conf_ctx_t结构体,不同的是可以有多个
 *   server{}区域,所以在server这一层有多少个server{}块就有多少个ngx_http_conf_ctx_t结构体。
 *
 *	 server指令对应的方法是/src/http/ngx_http_core_module.c/ngx_http_core_server()
 *	 TODO 内存结构图
 *
 *
 * 3.http核心模块(ngx_http_core_module)在遇到location指令后会创建ngx_http_conf_ctx_t结构体,
 *   有多少个location{}区域就会创建多少个ngx_http_conf_ctx_t结构体。
 *
 *   location指令对应的方法是/src/http/ngx_http_core_module.c/ngx_http_core_location()
 *	 TODO 内存结构图
 *
 *
 * http核心模块(ngx_http_core_module)本身使用ngx_http_core_main_conf_t、ngx_http_core_srv_conf_t
 * 、ngx_http_core_loc_conf_t这个三个配置信息结构体来关联各个http模块的配置信息结构体,因为会有多个server{}
 * 和location{},所以也会有多个ngx_http_core_srv|loc_conf_t结构体。
 *
 */
typedef struct {

	/*
	 * 存储http模块在http{}区域的配置信息
	 *
	 *  main_conf
	 *   -----
	 *   | * |
	 *   -----
	 *   \
	 *    -----------------
	 *    | * | * | * | 各个http模块在main块位置存放各自配置信息结构体的下标
	 *    -----------------
	 *
	 */
    void        **main_conf;

    /*
     * 存储http模块在server{}区域的配置信息
     */
    void        **srv_conf;

    /*
     * 存储http模块在location{}区域的配置信息
     */
    void        **loc_conf;
} ngx_http_conf_ctx_t;


typedef struct {
    ngx_int_t   (*preconfiguration)(ngx_conf_t *cf);
    ngx_int_t   (*postconfiguration)(ngx_conf_t *cf);

    void       *(*create_main_conf)(ngx_conf_t *cf);
    /*
     * conf: 该模块在本级区域创建的配置信息结构体(已经赋值完毕)
     */
    char       *(*init_main_conf)(ngx_conf_t *cf, void *conf);

    void       *(*create_srv_conf)(ngx_conf_t *cf);
    /*
     * prev: 该模块在上级区域创建的配置信息结构体(已经赋值完毕)
     * conf: 该模块在本级区域创建的配置信息结构体(已经赋值完毕)
     */
    char       *(*merge_srv_conf)(ngx_conf_t *cf, void *prev, void *conf);

    void       *(*create_loc_conf)(ngx_conf_t *cf);
    char       *(*merge_loc_conf)(ngx_conf_t *cf, void *prev, void *conf);
} ngx_http_module_t;


#define NGX_HTTP_MODULE           0x50545448   /* "HTTP" */

// 出现在http{}块内的配置信息
#define NGX_HTTP_MAIN_CONF        0x02000000
// 出现在server{}块内的配置信息
#define NGX_HTTP_SRV_CONF         0x04000000
// 出现在location{}块内的配置信息
#define NGX_HTTP_LOC_CONF         0x08000000
// 出现在 upstream{}块的配置信息
#define NGX_HTTP_UPS_CONF         0x10000000
// 出现在 server{}内的if{}内的配置信息
#define NGX_HTTP_SIF_CONF         0x20000000
// 出现在 location{}块内的if{}内的配置信息
#define NGX_HTTP_LIF_CONF         0x40000000
// TODO
#define NGX_HTTP_LMT_CONF         0x80000000


#define NGX_HTTP_MAIN_CONF_OFFSET  offsetof(ngx_http_conf_ctx_t, main_conf)
#define NGX_HTTP_SRV_CONF_OFFSET   offsetof(ngx_http_conf_ctx_t, srv_conf)
#define NGX_HTTP_LOC_CONF_OFFSET   offsetof(ngx_http_conf_ctx_t, loc_conf)


#define ngx_http_get_module_main_conf(r, module)                             \
    (r)->main_conf[module.ctx_index]
#define ngx_http_get_module_srv_conf(r, module)  (r)->srv_conf[module.ctx_index]
#define ngx_http_get_module_loc_conf(r, module)  (r)->loc_conf[module.ctx_index]


#define ngx_http_conf_get_module_main_conf(cf, module)                        \
    ((ngx_http_conf_ctx_t *) cf->ctx)->main_conf[module.ctx_index]
#define ngx_http_conf_get_module_srv_conf(cf, module)                         \
    ((ngx_http_conf_ctx_t *) cf->ctx)->srv_conf[module.ctx_index]
#define ngx_http_conf_get_module_loc_conf(cf, module)                         \
    ((ngx_http_conf_ctx_t *) cf->ctx)->loc_conf[module.ctx_index]

#define ngx_http_cycle_get_module_main_conf(cycle, module)                    \
    (cycle->conf_ctx[ngx_http_module.index] ?                                 \
        ((ngx_http_conf_ctx_t *) cycle->conf_ctx[ngx_http_module.index])      \
            ->main_conf[module.ctx_index]:                                    \
        NULL)


#endif /* _NGX_HTTP_CONFIG_H_INCLUDED_ */
