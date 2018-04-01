
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * ngx中的脚本引擎
 *
 * -----------------------------------脚本引擎在复杂值中的使用-------------------------
 * 1.ngx中的复杂值可以是文本、变量以他们的组合形式,比如下面的几个值都可以是一个复杂值
 *     "I am a complex value"
 *     "$complex_value"
 *     "I am a complex $value"
 *
 * 2.在ngx中使用ngx_http_compile_complex_value_t结构体表示一个原生的复杂值,这个原生的结构体需要通过
 *   ngx_http_compile_complex_value()方法把复杂值编译到ngx_http_complex_value_t结构体中,后续通
 *   过脚本引擎去执行这段脚本就可以了
 *
 *
 * -----------------------------------纯文本复杂值在脚本引擎中使用使用的例子-------------------------
 *【设置复杂值脚本code】
 * 1.有如下return指令
 *     return 200 "I am a complex value";
 *
 * 2.ngx在解析到return指令后执行ngx_http_rewrite_return()方法,该方法会向lcf->codes中添加一个脚本code
 *   ngx_http_script_return_code_t,这是一个专门为return指令设计的脚本code结构体,这个结构体包含了三个字段:
 *      code: 一个ngx_http_script_code_pt类型的方法变量,ngx中每个脚本code结构体的第一个字段必须是它.
 *            它在这里作用是执行return指令要做的动作,对应ngx_http_script_return_code()方法
 *      status: 用来存放code方式执行后的一些返回状态码
 *      text: 一个ngx_http_complex_value_t类型的变量,该字段的作用是存放return指令中复杂值编译后的值,因为
 *            return的返回值使用这个字段中的脚本code实时计算出来的
 *
 * 3.把复杂值先用ngx_http_compile_complex_value_t结构体表示,然后在使用ngx_http_compile_complex_value()
 *   方法把他编译到ngx_http_complex_value_t对象中
 *
 * 4.ngx中对复杂值是纯文本的编译是不需要生成脚本code的,所以编译后ngx_http_complex_value_t对象中的flushes、lengths
 *   和values三个字段都是NULL,只有value字段代表这个纯文本值
 *
 *【执行复杂值脚本code】
 * 1.return指令的执行是通过ngx_http_rewrite_handler()方法触发的,该方法通过脚本引擎执行lcf->codes中的脚本code
 *
 * 2.return指令放到lcf->codes中的脚本code是ngx_http_script_return_code_t对象,该对象中code字段对应的方法是
 *   ngx_http_script_return_code(),在该方法中会调用ngx_http_send_response()方法,并把编译好的脚本code传递给
 *   这个方法.(脚本code就是【设置复杂值脚本code】中第3步的ngx_http_complex_value_t对象)
 *
 * 3.在ngx_http_send_response()方法中通过调用ngx_http_complex_value()方法来获取真正的复杂值
 *
 * 4.因为当前复杂值是纯文本值,所以ngx_http_complex_value()方法会直接返回该文本值
 *
 *
 * -----------------------------------混合复杂值在脚本引擎中使用使用的例子-------------------------
 *【设置复杂值脚本code】
 * 1.有如下return指令
 *     return 200 "I am a complex $value";
 *
 * 2.ngx在解析到return指令后执行ngx_http_rewrite_return()方法,该方法会向lcf->codes中添加一个脚本code
 *   ngx_http_script_return_code_t.
 *
 * 3.复杂值先用ngx_http_compile_complex_value_t结构体表示,然后需要使用ngx_http_compile_complex_value()
 *   方法把他编译到ngx_http_complex_value_t对象中
 *
 * 4.编译的时候会把这个复杂值分成两部分,分别是
 *     "I am a complex "
 *     "$value"
 *   这两个部分都会对应各自的脚本code
 *      ngx_http_script_copy_code_t 对应文本值脚本code
 *      ngx_http_script_var_code_t  对应变量值脚本code
 *   其中每种脚本code都会被放到complex_value_t对象lengths字段和values字段中.
 *
 *   lengths中的脚本code对应的方法是用来计算复杂值的长度
 *   values中的脚本code对应的方法是用来计算复杂值的实际数据
 *
 *【执行复杂值脚本code】
 * 1.return指令的执行是通过ngx_http_rewrite_handler()方法触发的,该方法通过脚本引擎执行lcf->codes中的脚本code
 *
 * 2.return指令放到lcf->codes中的脚本code是ngx_http_script_return_code_t对象,该对象中code字段对应的方法是
 *   ngx_http_script_return_code(),在该方法中会调用ngx_http_send_response()方法,并把编译好的脚本code传递给
 *   这个方法.(脚本code就是【设置复杂值脚本code】中第3步的ngx_http_complex_value_t对象)
 *
 * 3.在ngx_http_send_response()方法中通过调用ngx_http_complex_value()方法来获取真正的复杂值
 *
 * 4.ngx_http_complex_value()方法通过执行脚本引擎来获取复杂值,用脚本引擎执行complex_value->lengths中的脚本code
 *   计算出复杂值的长度,然后用脚本引擎执行complex_value->values中的脚本code计算出复杂值的实际值
 *
 *
 * -------------------------------------总结-----------------------------------
 * ngx在编译复杂值的时候,会把复杂值分成几种不同的成分,然后每种成分都会对应一个生成脚本code的方法,比如
 *     ngx_http_script_add_copy_code(): 生成复杂值中纯文本脚本code(ngx_http_script_copy_code_t)的方法
 *     ngx_http_script_add_var_code(): 生成复杂值中变量脚本code(ngx_http_script_var_code_t)的方法
 *     ngx_http_script_add_args_code():
 *     ngx_http_script_add_capture_code(): 复杂值中包含正则的子捕获时,使用这个方法生成对应的脚本code
 *     ngx_http_script_add_full_name_code():
 * 然后每个code()方法又会对应相关的脚本code结构体,比如
 *     ngx_http_script_add_copy_code()对应的ngx_http_script_copy_code_t结构体
 *     ngx_http_script_add_var_code()对应的ngx_http_script_var_code_t结构体
 *
 *
 * 复杂值的编译ngx_http_compile_complex_value()方法发起,最终由ngx_http_script_compile()方法来完成
 * 一个复杂值中的文本值
 *    *sc->lengths
 *       ngx_http_script_copy_code_t
 *           code = (ngx_http_script_code_pt) ngx_http_script_copy_len_code; // 计算文本值长度
 *           len = len; // 文本值的长度
 *
 *    *sc->values
 *       ngx_http_script_copy_code_t + len + xxx
 *           code = ngx_http_script_copy_code  // 文本值拷贝到脚本引擎中
 *           len = len;
 *
 * 一个复杂值中的变量值
 *    *sc->lengths
 *       ngx_http_script_var_code_t
 *           code = (ngx_http_script_code_pt) ngx_http_script_copy_var_len_code; // 计算变量值长度(变量真正代表值的长度)
 *           index = index // 变量在ngx中的索引值
 *
 *   *sc->values
 *       ngx_http_script_var_code_t
 *           code = ngx_http_script_copy_var_code;  // 通过变量索引获取变量值,然后将变量值拷贝到脚本引擎中
 *           index = (uintptr_t) index; // 变量在ngx中的索引值 *
 *
 *
 * -----------------------------------------------------其它--------------------------------------
 * rewrite模块中的大部分指令是用脚本执行的
 *     ngx_http_script_break_code(): 执行break指令的脚本code
 *     ngx_http_script_return_code(): 执行return指令的脚本code
 *     ngx_http_script_if_code(): 执行if指令的脚本code
 *     ngx_http_script_regex_start_code(): 执行rewrite指令的时候会用到这个脚本code
 *
 *
 *
 *
 */

#ifndef _NGX_HTTP_SCRIPT_H_INCLUDED_
#define _NGX_HTTP_SCRIPT_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
	/*
	 * 引擎执行时存放codes中的元素
	 * 在变量值解析过程中设置的各种结构体,这些结构体的第一个字段必须是一个方法,比如下面的方法
	 *    void (*ngx_http_script_code_pt) (ngx_http_script_engine_t *e);
     *    size_t (*ngx_http_script_len_code_pt) (ngx_http_script_engine_t *e);
     * 脚本引擎像下面的方式执行:
     *   while (*(uintptr_t *) e->ip) {
     *     code = *(ngx_http_script_code_pt *) e->ip;
     *     code(e);
     *   }
     * 可以看到脚本在执行时会先把e->ip强转成约定好的方法,然后执行
	 */
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

    /*
     * 响应码
     */
    ngx_int_t                   status;

    /*
     * 当前被处理的请求
     */
    ngx_http_request_t         *request;
} ngx_http_script_engine_t;


typedef struct {
    ngx_conf_t                 *cf;
    /*
     * 要编译的复杂值,比如:
     *    return 200 "I am $uri";
     * 此时该字段指向"I am $uri"这个复杂值
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
     *						| 9 | 3 |
     *						---------
     *
     * 该字段用来存放某个复杂值中的变量在ngx中的索引值,比如
     *    "I am $uri $uri"
     * 上面的复杂值中存在两个变量,都是uri,那么该数组就顺序存储这两个变量值在ngx中的索引值
     *
     * 在调用ngx_http_script_add_var_code()方法时设置该字段中的值,一旦在复杂值中发现
     * 一个变量就会调用这个方法
     */
    ngx_array_t               **flushes;

    /*
     * 存放用来计算复杂值(存在变量的复杂值)长度的脚本,比如有如下复杂值
     *    reutrn 200 "I am $uri"
     * 那么此时该字段存放的脚本及其顺序是
     *    -------------------------------
     *    | ngx_http_script_copy_code_t |
     *    -------------------------------
     *    |  ngx_http_script_var_code_t |
     *    -------------------------------
     * 其中ngx_http_script_copy_code_t结构体有如下两个字段
     *    code = (ngx_http_script_code_pt) ngx_http_script_copy_len_code;
     *    len = len;
     * 此时code对应的方法用来计算文本"I am "的长度,其实就是len字段的值
     *
     * 其中ngx_http_script_var_code_t结构体也有如下两个字段
     *    code = code = (ngx_http_script_code_pt) ngx_http_script_copy_var_len_code;
     *    index = index; // 变量在ngx中的索引值
     * 此时code对应的方法用来计算变量"$uri"的长度(变量真正代表的值)
     *
     */
    ngx_array_t               **lengths;

    /*
     * lcf->codes
     *
     * 存放用来计算真正复杂值(存在变量的复杂值)的脚本,比如有如下复杂值
     *    reutrn 200 "I am $uri"
     * 那么此时该字段存放的脚本及其顺序是
     *    -------------------------------------------
     *    | ngx_http_script_copy_code_t + len + xxx |
     *    -------------------------------------------
     *    | ngx_http_script_var_code_t              |
     *    -------------------------------------------
     * 其中ngx_http_script_copy_code_t结构体有两个字段
     *    code = ngx_http_script_copy_code;
     *    len = len;
     * 此时code对应的方法的作用是将文本值"I am "拷贝到脚本引擎中,而这个文本值就放在ngx_http_script_copy_code_t
     * 结构体后面,也就是图中len所占的字节个数
     *
     * 其中ngx_http_script_var_code_t结构体也有如下两个字段
     *    code = ngx_http_script_copy_var_code;
     *    index = (uintptr_t) index;
     * 此时code对应的方法会通过变量的索引值index从ngx中取出,然后将其拷贝到脚本引擎中
     *
     * 字段lengths里面的脚本是用来计算复杂值长度的
     * 字段values里面的脚本是用来计算真正复杂值的
     */
    ngx_array_t               **values;

    /*
     * 复杂值中的变量个数, 比如:
     *    set $a $bb$cc
     * 则该值为2
     *
     * 比如:
     *    return 200 "I am $uri";
     * 该该值为1
     */
    ngx_uint_t                  variables;
    ngx_uint_t                  ncaptures;
    ngx_uint_t                  captures_mask;

    /*
     *
     */
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


/*
 * 复杂值编译后的结构体
 * 该结构体用来在运行时计算表达式的值
 */
typedef struct {
	/*
	 * 原生复杂值,比如有如下指令
	 *    return 200 "I am $uri $host";
	 * 那么该字段值为"I am $uri $host"
	 */
    ngx_str_t                   value;

    /*
     * 该字段用来存放某个复杂值中的变量在ngx中的索引值,比如
     *    "I am $uri $uri"
     * 上面的复杂值中存在两个变量,都是uri,那么该数组就顺序存储这两个变量值在ngx中的索引值
     *
     * 数组中存放各个变量在cmcf->variables中的下标
     * 		flushes
     * 		-------
     * 		|  *  |
     * 		-------
     *		    \
     *		    ---------------
     *			| ngx_array_t |
     *			---------------
     *					\ elts
     *					---------
     *					| 9 | 3 |
     *					---------
     *
     * 在调用ngx_http_script_add_var_code()方法时设置该字段中的值,一旦在复杂值中发现
     * 一个变量就会调用这个方法
     *
     */
    ngx_uint_t                 *flushes;

    /*
     * 当变量值是纯文本时该值是NULL,比如有如下指令
     *    return 200 "I am value"
     * 此时该字段值为NULL
     *
     *
     * 存放用来计算复杂值(存在变量的复杂值)长度的脚本,比如有如下复杂值
     *    reutrn 200 "I am $uri"
     * 那么此时该字段存放的脚本及其顺序是
     *    -------------------------------
     *    | ngx_http_script_copy_code_t |
     *    -------------------------------
     *    |  ngx_http_script_var_code_t |
     *    -------------------------------
     * 其中ngx_http_script_copy_code_t结构体有如下两个字段
     *    code = (ngx_http_script_code_pt) ngx_http_script_copy_len_code;
     *    len = len;
     * 此时code对应的方法用来计算文本"I am "的长度,其实就是len字段的值
     *
     * 其中ngx_http_script_var_code_t结构体也有如下两个字段
     *    code = code = (ngx_http_script_code_pt) ngx_http_script_copy_var_len_code;
     *    index = index // 变量在ngx中的索引值
     * 此时code对应的方法用来计算变量"$uri"的长度(变量真正代表的值)
     *
     */
    void                       *lengths;

    /*
     * lcf->codes
     *
     * 存放用来计算真正复杂值(存在变量的复杂值)的脚本,比如有如下复杂值
     *    reutrn 200 "I am $uri"
     * 那么此时该字段存放的脚本及其顺序是
     *    -------------------------------------------
     *    | ngx_http_script_copy_code_t + len + xxx |
     *    -------------------------------------------
     *    | ngx_http_script_var_code_t              |
     *    -------------------------------------------
     * 其中ngx_http_script_copy_code_t结构体有两个字段
     *    code = ngx_http_script_copy_code;
     *    len = len;
     * 此时code对应的方法的作用是将文本值"I am "拷贝到脚本引擎中,而这个文本值就放在ngx_http_script_copy_code_t
     * 结构体后面,也就是图中len所占的字节个数
     *
     * 其中ngx_http_script_var_code_t结构体也有如下两个字段
     *    code = ngx_http_script_copy_var_code;
     *    index = (uintptr_t) index;
     * 此时code对应的方法会通过变量的索引值index从ngx中取出,然后将其拷贝到脚本引擎中
     *
     * 字段lengths里面的脚本是用来计算复杂值长度的
     * 字段values里面的脚本是用来计算真正复杂值的
     */
    void                       *values;
} ngx_http_complex_value_t;


/*
 * 用来编译复杂值的中间结构体
 *
 * 在配置阶段收集复杂值,然后把复杂值编译到ngx_http_complex_value_t结构体的一个中间结构体
 * 作为ngx_http_compile_complex_value()方法的一个入参,最终会把值编译到complex_value字段中
 */
typedef struct {
    ngx_conf_t                 *cf;

    /*
     * 原生复杂值,比如
     *    return 200 "I am $uri"
     * 则该值为
     *    "I am $uri"
     */
    ngx_str_t                  *value;
    /*
     * 原生复杂值被编译后的值
     */
    ngx_http_complex_value_t   *complex_value;

    unsigned                    zero:1;
    unsigned                    conf_prefix:1;
    unsigned                    root_prefix:1;
} ngx_http_compile_complex_value_t;


typedef void (*ngx_http_script_code_pt) (ngx_http_script_engine_t *e);
typedef size_t (*ngx_http_script_len_code_pt) (ngx_http_script_engine_t *e);


/*
 * 用于计算纯文本的长度和拷贝纯文本值到脚本引擎中的脚本结构体,比如有如下复杂值
 *     return 200 "I am $uri";
 *
 * 在计算复杂值中纯文本值("I am ")的长度时候,该结构体的code对应
 *    code = (ngx_http_script_code_pt) ngx_http_script_copy_len_code;
 * 而该方法的作用就是在引擎中跳过结构体,然后返回该结构体对应的len字段
 *     ngx_http_script_copy_code_t  *code;
 *
 *     code = (ngx_http_script_copy_code_t *) e->ip;
 *     e->ip += sizeof(ngx_http_script_copy_code_t);
 *
 *     return code->len;
 *
 *
 * 在计算复杂值中纯文本("I am ")的实际文本值的时候,该结构体的code对应
 *     code = ngx_http_script_copy_code
 * 该方法的作用是在引擎中跳过该结构体和文本信息,然后将文本值拷贝到引擎中
 *     code = (ngx_http_script_copy_code_t *) e->ip;
 *
 *     p = e->pos;
 *
 *     if (!e->skip) { // 文本信息拷贝到引擎中
 *        e->pos = ngx_copy(p, e->ip + sizeof(ngx_http_script_copy_code_t), code->len);
 *     }
 *
 *     e->ip += sizeof(ngx_http_script_copy_code_t) + ((code->len + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
 */
typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   len;
} ngx_http_script_copy_code_t;


/*
 * 用来计算复杂值中变量值的脚本,比如有如下复杂值
 *     return 200 "I am $uri";
 *
 * 在计算复杂值中变量("$uri")的长度时候,该结构体的code对应
 *     code = (ngx_http_script_code_pt) ngx_http_script_copy_var_len_code;
 * 此时code对应的方法用来计算变量("$uri")的长度(变量真正代表的值)
 *
 * 在计算复杂值中变量("$uri")的值的时候,该结构体的code对应
 *     code = ngx_http_script_copy_var_code;
 * 此时code对应的方法会将变量的索引值index从ngx中取出,然后将其拷贝到脚本引擎中
 *
 */
typedef struct {
    ngx_http_script_code_pt     code;

    /*
     * 当前变量在cmcf->variables数组中的下标
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
 *
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

    /*
     * 调用ngx_http_compile_complex_value()方法编译好的复杂值
     */
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
