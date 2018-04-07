
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
 *  cycle->conf_ctx
 *   -----
 *   | * |
 *   -----
 *    \            ngx_http_module.index
 *     -----------------
 *     | * | * | * | * |
 *     -----------------
 *                    \
 *                   -----------------------
 *                   | ngx_http_conf_ctx_t |
 *                   -----------------------
 *
 *
 * 2.http核心模块(ngx_http_core_module)在遇到server指令后会创建ngx_http_conf_ctx_t结构体,
 *   同http{}区域一样,一个server{}区域也会对应一个ngx_http_conf_ctx_t结构体,不同的是可以有多个
 *   server{}区域,所以在server这一层有多少个server{}块就有多少个ngx_http_conf_ctx_t结构体。
 *
 *   server指令对应的方法是/src/http/ngx_http_core_module.c/ngx_http_core_server()
 *   TODO 内存结构图
 *
 *
 * 3.http核心模块(ngx_http_core_module)在遇到location指令后会创建ngx_http_conf_ctx_t结构体,
 *   有多少个location{}区域就会创建多少个ngx_http_conf_ctx_t结构体。
 *
 *   location指令对应的方法是/src/http/ngx_http_core_module.c/ngx_http_core_location()
 *   TODO 内存结构图
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
     *    \
     *    -----------------
     *    | * | * | * | 各个http模块在main块位置存放各自配置信息结构体的下标
     *    -----------------
     *
     * 常规情况下,所有http指令,只要最深出现在http{}块中,那么该指令信息在解析后就应该存放在其对应的main级别
     * 的结构体中,比如如下配置:
     *        http {
     *             myhttp aa;
     *
     *             server{
     *
     *                 location {
     *                     // something
     *                 }
     *             }
     *         }
     * 配置中的指令信息会存放在http{}块对应的myhttp_main_conf结构体中。
     *
     */
    void        **main_conf;

    /*
     * 存储http模块在server{}区域的配置信息
     *
     * 常规情况下,所有http指令,只要最深出现在server{}块中,那么该指令信息在解析后就都应该存放在其模块对应的srv
     * 级别的结构体中,比如如下配置:
     *         http {
     *             myhttp aa;
     *
     *             server {
     *                     myhttp bb;
     *
     *                     location {
     *                         // something
     *                     }
     *             }
     *         }
     * 对于上面出现的两条同样的指令,他们都会存放在各自myhttp_srv_conf结构体中。第一条指令信息存放在http{}
     * 块中对应的myhttp_srv_conf结构体中; 第二条指令存放在server{}块中对应对的myhttp_srv_conf结构体中;
     */
    void        **srv_conf;

    /*
     * 存储所有http模块在location{}区域的配置信息
     *
     * 常规情况下,所有的http指令,只要最深可以出现在location{}块中,那么该指令信息在解析后就都应该存放在其模块
     * 对应的loc级别的结构体中,比如有如下配置:
     *         http {
     *             myhttp aa;
     *
     *             server {
     *                 myhttp bb;
     *
     *                 location {
     *                     myhttp cc;
     *                 }
     *             }
     *         }
     * 上面的配置总共出现了三条同样的指令,只是参数值不一样,但是因为该指令可以出现在locaton{}块中,所以解析后的信息
     * 都应该存放在loc级别的myhttp_loc_conf结构体中。
     * 所以,对于第一条指令信息,应该存放在http{}块对应的myhttp_loc_conf结构体中;第二条指令存放在server{}块对应
     * 的myhttp_loc_conf结构体中;第三条指令存放在location{}块对应的myhttp_loc_conf结构体中;
     */
    void        **loc_conf;
} ngx_http_conf_ctx_t;


typedef struct {

    /*
     * 在ngx_http_block()方法中,当调用完毕http{}块中涉及的create_main|srv|loc_conf()方法后调用该方法,
     * 此时只是创建了各个http模块在http{}块中的配置信息结构体,指令还没有开始解析
     *
     * 这一步发生在执行ngx_http_init_phases()方法之前,该方法用来初始化各个阶段用来存放注册handler的数组,
     * 所以向http各个阶段中注册自己的方法,在此时是行不通的.
     */
    ngx_int_t   (*preconfiguration)(ngx_conf_t *cf);
    /*
     * 在ngx_http_block()方法中,当所有指令解析完毕,并且也初始化和合并完毕后执行.
     *
     * 这一步发生在ngx_http_init_phases()方法之后,当ngx_http_init_phases()方法执行完后,存放各个阶段handlr的
     * 数组也就初始化完毕了,所以这一步就可以把自己的handler注册到相应的阶段了.
     *
     * 这一步发生在ngx_http_init_phase_handlers()方法之前,ngx_http_init_phase_handlers()方法的作用是把用户
     * 注册到cmcf->phases[i].handlers数组中的handler()方法,放到cmcf->phase_engine.handlers阶段引擎中.所以
     * 这一步要在ngx_http_init_phase_handlers()方法之前执行.
     */
    ngx_int_t   (*postconfiguration)(ngx_conf_t *cf);


    /*
     * 在preconfiguration()方法之前调用,此时指令还没有开始解析
     * 当遇见http{}时,会调用所有http模块的这个方法.
     *
     */
    void       *(*create_main_conf)(ngx_conf_t *cf);
    /*
     * 在http{}块内的指令解析完毕后调用,用来做一些初始化操作
     *
     * conf: 该模块在本级区域创建的配置信息结构体(已经赋值完毕)
     */
    char       *(*init_main_conf)(ngx_conf_t *cf, void *conf);


    /*
     * 每遇见一个server{}块都会通过ngx_http_core_server()方法来调用所有http模块的这个方法,因为
     * ngx_http_core_module.c也是http模块,所以其对应的ngx_http_core_create_srv_conf()发就会调用,
     * 从而产生一个代表server{}块的ngx_http_core_srv_conf_t结构体
     *
     * 另外在http{}块中为了存放server{}块的一些公共配置,在http{}块对应的方法ngx_http_block()中也会调用所有http模块
     * 的create_srv_conf()方法,比如对于ngx_http_core_module.c这个http模块来说对应ngx_http_core_create_srv_conf()
     * 方法,他对应的结构体是ngx_http_core_srv_conf_t. 这种形式可以理解为http_core模块在http{}块中放了一个结构体,而这个
     * 结构体是通过create_srv_conf()方法产生的,当然其它模块也可以通过该方法在http{}块存放结构体.
     *
     * 实际上ngx为所有http模块预留了三个位置用来存放结构体,分别是http{}、server{}、location{}.每一个http模块又预留了三个
     * 方法(create_main|srv|loc_conf)来创建不同位置的结构体,比如某个http模块的指令可以出现在任何位置中,那这个模块就可以实现
     * create_main|srv|loc_conf()这三个方法,如果只能在其中一个位置出现,那就实现其中一个方法就可以了,同时上下级之间有继承关系
     */
    void       *(*create_srv_conf)(ngx_conf_t *cf);
    /*
     * 对应的http模块在server{}块的指令信息解析完毕后调用方法,用来合并上级配置信息
     *
     * prev: 该模块在上级区域创建的配置信息结构体(已经赋值完毕)
     * conf: 该模块在本级区域创建的配置信息结构体(已经赋值完毕)
     */
    char       *(*merge_srv_conf)(ngx_conf_t *cf, void *prev, void *conf);


    /*
     * 每遇到一个location{}、if{}块等就会调用所有http模块的这个方法
     */
    void       *(*create_loc_conf)(ngx_conf_t *cf);
    /*
     * 对应的location{}块的指令信息解析完毕后调用该方法,用来合并上级配置信息
     */
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


#define ngx_http_get_module_main_conf(r, module)  (r)->main_conf[module.ctx_index]
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
