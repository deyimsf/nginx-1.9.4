
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_SCRIPT_H_INCLUDED_
#define _NGX_HTTP_SCRIPT_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
	// 引擎执行时存放codes中的元素
    u_char                     *ip;
    u_char                     *pos;

    /*
     * 引擎执行过程中携带当前变量值
     * 相当于基于栈的指令中的栈,ngx_http_rewrite_loc_conf_t->stack_size表示栈大小
     */
    ngx_http_variable_value_t  *sp;

    ngx_str_t                   buf;
    ngx_str_t                   line;

    /* the start of the rewritten arguments */
    u_char                     *args;

    unsigned                    flushed:1;
    unsigned                    skip:1;
    unsigned                    quote:1;
    unsigned                    is_args:1;
    unsigned                    log:1;

    ngx_int_t                   status;
    ngx_http_request_t         *request;
} ngx_http_script_engine_t;


typedef struct {
    ngx_conf_t                 *cf;
    /*
     * 复杂变量值,如 set $a $bb 中的$bb
     *
     * 一个表达式 TODO
     */
    ngx_str_t                  *source;

    /*
     * 数组中存放各个变量在cmcf->variables中的下标,比如set $a $bb$cc
     * 假设 $bb的下标是1, $cc的下标是2
     * 那么该字段的值如下:
     * 		flushes
     * 		-------
     * 		|  *  |
     * 		-------
     *			\
     *			-------
     *			|  *  |
     *			-------
     *				\
     *				---------------
     *				| ngx_array_t |
     *				---------------
     *						\ elts
     *						---------
     *						| 1 | 2 |
     *						---------
     */
    ngx_array_t               **flushes;
    ngx_array_t               **lengths;

    /*
     * lcf->codes
     */
    ngx_array_t               **values;

    /*
     * 复杂变量值中的变量个数, 比如:
     * 		set $a $bb$cc
     * 则该值为2
     */
    ngx_uint_t                  variables;
    ngx_uint_t                  ncaptures;
    ngx_uint_t                  captures_mask;
    ngx_uint_t                  size;

    void                       *main;

    unsigned                    compile_args:1;
    unsigned                    complete_lengths:1;
    unsigned                    complete_values:1;
    unsigned                    zero:1;
    unsigned                    conf_prefix:1;
    unsigned                    root_prefix:1;

    unsigned                    dup_capture:1;
    unsigned                    args:1;
} ngx_http_script_compile_t;


typedef struct {
	/*
	 * 变量值
	 * 如: $http_name
	 */
    ngx_str_t                   value;
    ngx_uint_t                 *flushes;
    void                       *lengths;
    void                       *values;
} ngx_http_complex_value_t;


typedef struct {
    ngx_conf_t                 *cf;

    // 变量值
    ngx_str_t                  *value;
    ngx_http_complex_value_t   *complex_value;

    unsigned                    zero:1;
    unsigned                    conf_prefix:1;
    unsigned                    root_prefix:1;
} ngx_http_compile_complex_value_t;


typedef void (*ngx_http_script_code_pt) (ngx_http_script_engine_t *e);
typedef size_t (*ngx_http_script_len_code_pt) (ngx_http_script_engine_t *e);


typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   len;
} ngx_http_script_copy_code_t;


typedef struct {
    ngx_http_script_code_pt     code;

    /*
     * 当前变量在cmcf->variables数组中的下标
     *
     */
    uintptr_t                   index;
} ngx_http_script_var_code_t;


typedef struct {
    ngx_http_script_code_pt     code;
    ngx_http_set_variable_pt    handler;
    uintptr_t                   data;
} ngx_http_script_var_handler_code_t;


typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   n;
} ngx_http_script_copy_capture_code_t;


#if (NGX_PCRE)

/*
 * rewrite指令(还有if语句中的正则遍历)对应的脚本引擎结构体
 * 每一个这样的结构体都会对应一个ngx_http_script_code_pt回调方法,这个方法有脚本引擎负责调用,
 * 其它字段根据指令对应的功能会做响应的调整
 */
typedef struct {
	// 脚本引擎要执行的方法
    ngx_http_script_code_pt     code;
    ngx_http_regex_t           *regex;
    ngx_array_t                *lengths;
    uintptr_t                   size;
    uintptr_t                   status;
    uintptr_t                   next;

    uintptr_t                   test:1;
    uintptr_t                   negative_test:1;
    uintptr_t                   uri:1;
    uintptr_t                   args:1;

    /* add the r->args to the new arguments */
    uintptr_t                   add_args:1;

    uintptr_t                   redirect:1;

    /*
     * rewrite指令携带是否携带了break参数,如果携带了则该值为1
     * 为1的时候表示无视uri的改变,这时候不会进行uri再匹配,阶段方法ngx_http_core_post_rewrite_phase()会直接返回,不会
     * 执行方法后面的逻辑。
     *
     * 当该标记为1的时候,rewrite指令对应的解析方法ngx_http_rewrite(),会向脚本引擎codes中添加终止标志,当脚本引擎检查到
     * 该标志后就不会继续向下执行了,所以该模块所有使用脚本引擎执行的指令就都不会被执行了,比如return指令。
     */
    uintptr_t                   break_cycle:1;

    ngx_str_t                   name;
} ngx_http_script_regex_code_t;


/*
 * rewrite指令(还有if语句中的正则遍历)对应的脚本引擎结构体
 * 每一个这样的结构体都会对应一个ngx_http_script_code_pt回调方法,这个方法有脚本引擎负责调用,
 * 其它字段根据指令对应的功能会做响应的调整
 */
typedef struct {
    ngx_http_script_code_pt     code;

    uintptr_t                   uri:1;
    uintptr_t                   args:1;

    /* add the r->args to the new arguments */
    uintptr_t                   add_args:1;

    uintptr_t                   redirect:1;
} ngx_http_script_regex_end_code_t;

#endif


typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   conf_prefix;
} ngx_http_script_full_name_code_t;


/*
 * return指令对应的脚本引擎结构体
 * 每一个这样的结构体都会对应一个ngx_http_script_code_pt回调方法,这个方法有脚本引擎负责调用,
 * 其它字段根据指令对应的功能会做响应的调整
 */
typedef struct {
	/*
	 * 脚本引擎要执行的方法
	 */
    ngx_http_script_code_pt     code;

    // return指令中的状态码
    uintptr_t                   status;
    ngx_http_complex_value_t    text;
} ngx_http_script_return_code_t;


typedef enum {
    ngx_http_script_file_plain = 0,
    ngx_http_script_file_not_plain,
    ngx_http_script_file_dir,
    ngx_http_script_file_not_dir,
    ngx_http_script_file_exists,
    ngx_http_script_file_not_exists,
    ngx_http_script_file_exec,
    ngx_http_script_file_not_exec
} ngx_http_script_file_op_e;


typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   op;
} ngx_http_script_file_code_t;


/*
 * if指令对应的脚本引擎结构体
 * 每一个这样的结构体都会对应一个ngx_http_script_code_pt回调方法,这个方法有脚本引擎负责调用,
 * 其它字段根据指令对应的功能会做响应的调整
 */
typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   next;
    /*
     * 如果当前if指令存在于一个locaton{}中,那么该字段就有值
     *
     * 也就是说if(){}块也会有自己的一份loc_conf数据
     */
    void                      **loc_conf;
} ngx_http_script_if_code_t;


/*
 * 复杂变量值,如: set $a $bb$cc
 *
 * 如果变量值中存在其它变量,则使用该结构体代替变量值
 * 变量值中没有其它变量才会使用ngx_http_script_value_code_t结构体
 *
 */
typedef struct {
	/*
	 * 用户处理复杂变量值的脚本函数(如:ngx_http_script_complex_value_code)
	 */
    ngx_http_script_code_pt     code;
    ngx_array_t                *lengths;
} ngx_http_script_complex_value_code_t;


/*
 * 脚本引擎中的值
 */
typedef struct {
	/*
	 * 处理变量值的函数,比如ngx_http_script_value_code()方法
	 */
    ngx_http_script_code_pt     code;

    /*
     * 变量值中变量的个数,0代表没有变量,比如：
     * 	 set $a bb;
     * 则value等于0,如果：
     * 	 set $a $cc
     * 则value等于1
     *
     */
    uintptr_t                   value;

    /*
     * 纯变量值长度,比如 set $a bb;
     * 则该值为2
     */
    uintptr_t                   text_len;
    /*
     * 纯变量值,比如 set $a bb;
     * 则该值为bb
     */
    uintptr_t                   text_data;
} ngx_http_script_value_code_t;


void ngx_http_script_flush_complex_value(ngx_http_request_t *r,
    ngx_http_complex_value_t *val);
ngx_int_t ngx_http_complex_value(ngx_http_request_t *r,
    ngx_http_complex_value_t *val, ngx_str_t *value);
ngx_int_t ngx_http_compile_complex_value(ngx_http_compile_complex_value_t *ccv);
char *ngx_http_set_complex_value_slot(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


ngx_int_t ngx_http_test_predicates(ngx_http_request_t *r,
    ngx_array_t *predicates);
char *ngx_http_set_predicate_slot(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

ngx_uint_t ngx_http_script_variables_count(ngx_str_t *value);
ngx_int_t ngx_http_script_compile(ngx_http_script_compile_t *sc);
u_char *ngx_http_script_run(ngx_http_request_t *r, ngx_str_t *value,
    void *code_lengths, size_t reserved, void *code_values);
void ngx_http_script_flush_no_cacheable_variables(ngx_http_request_t *r,
    ngx_array_t *indices);

void *ngx_http_script_start_code(ngx_pool_t *pool, ngx_array_t **codes,
    size_t size);
void *ngx_http_script_add_code(ngx_array_t *codes, size_t size, void *code);

size_t ngx_http_script_copy_len_code(ngx_http_script_engine_t *e);
void ngx_http_script_copy_code(ngx_http_script_engine_t *e);
size_t ngx_http_script_copy_var_len_code(ngx_http_script_engine_t *e);
void ngx_http_script_copy_var_code(ngx_http_script_engine_t *e);
size_t ngx_http_script_copy_capture_len_code(ngx_http_script_engine_t *e);
void ngx_http_script_copy_capture_code(ngx_http_script_engine_t *e);
size_t ngx_http_script_mark_args_code(ngx_http_script_engine_t *e);
void ngx_http_script_start_args_code(ngx_http_script_engine_t *e);
#if (NGX_PCRE)
void ngx_http_script_regex_start_code(ngx_http_script_engine_t *e);
void ngx_http_script_regex_end_code(ngx_http_script_engine_t *e);
#endif
void ngx_http_script_return_code(ngx_http_script_engine_t *e);
void ngx_http_script_break_code(ngx_http_script_engine_t *e);
void ngx_http_script_if_code(ngx_http_script_engine_t *e);
void ngx_http_script_equal_code(ngx_http_script_engine_t *e);
void ngx_http_script_not_equal_code(ngx_http_script_engine_t *e);
void ngx_http_script_file_code(ngx_http_script_engine_t *e);
void ngx_http_script_complex_value_code(ngx_http_script_engine_t *e);
void ngx_http_script_value_code(ngx_http_script_engine_t *e);
void ngx_http_script_set_var_code(ngx_http_script_engine_t *e);
void ngx_http_script_var_set_handler_code(ngx_http_script_engine_t *e);
void ngx_http_script_var_code(ngx_http_script_engine_t *e);
void ngx_http_script_nop_code(ngx_http_script_engine_t *e);


#endif /* _NGX_HTTP_SCRIPT_H_INCLUDED_ */
