
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


/*
 * 该模块涉及了三个阶段
 * 	NGX_HTTP_SERVER_REWRITE_PHASE  负责执行脚本引擎
 * 	NGX_HTTP_REWRITE_PHASE 负责执行脚本引擎
 * 	NGX_HTTP_POST_REWRITE_PHASE 负责阶段流转,继续下一个阶段还是重新config阶段
 *
 *
 * rewrite指令中的第三个参数会终止该模块的脚本引擎执行,所以该模块任何使用脚本引擎执行的指令都会被终止
 *
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
	/*
	 * typedef struct {
     *	  ngx_http_script_code_pt     code;
     *	  uintptr_t                   value;
     *	  uintptr_t                   text_len;
     *	  // 变量的值,这里把变量值指针转换为了long
     *	  uintptr_t                   text_data;
	 * } ngx_http_script_value_code_t;
	 *
	 * 存放处理变量值的结构体(ngx_http_script_value_code_t),比如:
	 *  set $name zhangsan
	 *
	 * 那么此时,对于变量name来说,codes的第一个元素就是ngx_http_script_value_code_t:
	 *  	zhangsan
	 *  (实际上codes中放的元素大小是1个字节,结构体ngx_http_script_value_code_t有多大,就占多少个元素)
	 *
	 *
	 * 假设有两个set指令,分别是
	 * 	set $a	aa;
	 * 	set $b 	bbc;
	 * 那么最终codes的部分内存结构如下:
	 *   ------------------------------------ <--- ngx_http_script_value_code_t
	 *   |code = ngx_http_script_value_code |
	 * 	 ------------------------------------
	 * 	 |value = 0							|
	 * 	 ------------------------------------
	 * 	 |text_len = 2						|
	 * 	 ------------------------------------
	 * 	 |text_data = aa					|
	 * 	 ------------------------------------ <--- ngx_http_script_var_code_t
	 * 	 |code=ngx_http_script_set_var_code |
	 *	 ------------------------------------
	 *	 |index = 1							|
	 *	 ------------------------------------ <--- ngx_http_script_value_code_t
	 *   |code = ngx_http_script_value_code |
	 * 	 ------------------------------------
	 * 	 |value = 0							|
	 * 	 ------------------------------------
	 * 	 |text_len = 3						|
	 * 	 ------------------------------------
	 * 	 |text_data = bbc					|
	 * 	 ------------------------------------ <--- ngx_http_script_var_code_t
	 * 	 |code=ngx_http_script_set_var_code |
	 *	 ------------------------------------
	 *	 |index = 2							|
	 *	 ------------------------------------
	 *
	 *
	 * 假设有个set指令如下:
	 *	 set $a $bb$cc
	 * 那么codes的部分内存结构如下 (TODO ?)
	 *   codes
	 *   -----
	 *   | * |
	 *   -----
	 *   	\   ngx_http_script_complex_value_code_t
	 *   	----------------------------------------------
	 *   	|code = ngx_http_script_complex_value_code() |
	 *   	----------------------------------------------
	 *   	|				   *lengths					 |
	 *		----------------------------------------------
	 *
	 */
    ngx_array_t  *codes;        /* uintptr_t */

    /*
     * 表示栈大小(ngx_http_script_engine_t->sp)
     * ngx_http_script_engine_t->sp可以看做是栈
     */
    ngx_uint_t    stack_size;

    ngx_flag_t    log;
    ngx_flag_t    uninitialized_variable_warn;
} ngx_http_rewrite_loc_conf_t;


static void *ngx_http_rewrite_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_rewrite_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_rewrite_init(ngx_conf_t *cf);
static char *ngx_http_rewrite(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_rewrite_return(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_rewrite_break(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_rewrite_if(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char * ngx_http_rewrite_if_condition(ngx_conf_t *cf,
    ngx_http_rewrite_loc_conf_t *lcf);
static char *ngx_http_rewrite_variable(ngx_conf_t *cf,
    ngx_http_rewrite_loc_conf_t *lcf, ngx_str_t *value);
static char *ngx_http_rewrite_set(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char * ngx_http_rewrite_value(ngx_conf_t *cf,
    ngx_http_rewrite_loc_conf_t *lcf, ngx_str_t *value);


static ngx_command_t  ngx_http_rewrite_commands[] = {

    { ngx_string("rewrite"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_TAKE23,
      ngx_http_rewrite,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("return"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_TAKE12,
      ngx_http_rewrite_return,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("break"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_NOARGS,
      ngx_http_rewrite_break,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("if"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_BLOCK|NGX_CONF_1MORE,
      ngx_http_rewrite_if,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("set"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_TAKE2,
      ngx_http_rewrite_set,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("rewrite_log"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF
                        |NGX_HTTP_LIF_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_rewrite_loc_conf_t, log),
      NULL },

    { ngx_string("uninitialized_variable_warn"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF
                        |NGX_HTTP_LIF_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_rewrite_loc_conf_t, uninitialized_variable_warn),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_rewrite_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_rewrite_init,                 /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_rewrite_create_loc_conf,      /* create location configuration */
    ngx_http_rewrite_merge_loc_conf        /* merge location configuration */
};


ngx_module_t  ngx_http_rewrite_module = {
    NGX_MODULE_V1,
    &ngx_http_rewrite_module_ctx,          /* module context */
    ngx_http_rewrite_commands,             /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/*
 * 执行脚本引擎
 *
 * 假设有如下指令:
 * 	set $a abc
 *
 * 用ngx_http_script_value_code()方法将值(abc)压栈(e->sp++)
 * 用ngx_http_script_set_var_code()方法弹栈(e->sp--)取值,并放到对应的r->variables中
 */
static ngx_int_t
ngx_http_rewrite_handler(ngx_http_request_t *r)
{
    ngx_int_t                     index;
    ngx_http_script_code_pt       code;
    ngx_http_script_engine_t     *e;
    ngx_http_core_srv_conf_t     *cscf;
    ngx_http_core_main_conf_t    *cmcf;
    ngx_http_rewrite_loc_conf_t  *rlcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
    cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);
    index = cmcf->phase_engine.location_rewrite_index;

    if (r->phase_handler == index && r->loc_conf == cscf->ctx->loc_conf) {
        /* skipping location rewrite phase for server null location */
        return NGX_DECLINED;
    }

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_rewrite_module);

    if (rlcf->codes == NULL) {
        return NGX_DECLINED;
    }

    // 创建脚本引擎内存空间
    e = ngx_pcalloc(r->pool, sizeof(ngx_http_script_engine_t));
    if (e == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    // 相当于基于栈的指令中的栈,rlcf->stack_size表示栈大小
    e->sp = ngx_pcalloc(r->pool,
                        rlcf->stack_size * sizeof(ngx_http_variable_value_t));
    if (e->sp == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    e->ip = rlcf->codes->elts;
    e->request = r;
    e->quote = 1;
    e->log = rlcf->log;
    e->status = NGX_DECLINED;

    while (*(uintptr_t *) e->ip) {
        code = *(ngx_http_script_code_pt *) e->ip;
        code(e);

        // ngx_http_script_value_code_t->ngx_http_script_value_code()
        // ngx_http_script_var_code_t->ngx_http_script_set_var_code()
    }

    /*
     * 脚本执行完毕之后会根据e->status值来决定返回什么数据
     */

    if (e->status < NGX_HTTP_BAD_REQUEST) {
    	/*
    	 * 状态码小于400,则表明为发生错误,走正常逻辑
    	 */
        return e->status;
    }

    if (r->err_status == 0) {

    	/*
    	 * 请求错误码为0代表未发生错误,按照原样返回状态码
    	 */

        return e->status;
    }

    return r->err_status;
}


/*
 * 返回未初始化变量的值
 */
static ngx_int_t
ngx_http_rewrite_var(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_http_variable_t          *var;
    ngx_http_core_main_conf_t    *cmcf;
    ngx_http_rewrite_loc_conf_t  *rlcf;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_rewrite_module);

    if (rlcf->uninitialized_variable_warn == 0) {
        *v = ngx_http_variable_null_value;
        return NGX_OK;
    }

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);

    var = cmcf->variables.elts;

    /*
     * the ngx_http_rewrite_module sets variables directly in r->variables,
     * and they should be handled by ngx_http_get_indexed_variable(),
     * so the handler is called only if the variable is not initialized
     */

    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                  "using uninitialized \"%V\" variable", &var[data].name);

    *v = ngx_http_variable_null_value;

    return NGX_OK;
}


static void *
ngx_http_rewrite_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_rewrite_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_rewrite_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->stack_size = NGX_CONF_UNSET_UINT;
    conf->log = NGX_CONF_UNSET;
    conf->uninitialized_variable_warn = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_rewrite_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_rewrite_loc_conf_t *prev = parent;
    ngx_http_rewrite_loc_conf_t *conf = child;

    uintptr_t  *code;

    ngx_conf_merge_value(conf->log, prev->log, 0);
    ngx_conf_merge_value(conf->uninitialized_variable_warn,
                         prev->uninitialized_variable_warn, 1);
    ngx_conf_merge_uint_value(conf->stack_size, prev->stack_size, 10);

    if (conf->codes == NULL) {
        return NGX_CONF_OK;
    }

    if (conf->codes == prev->codes) {
        return NGX_CONF_OK;
    }

    code = ngx_array_push_n(conf->codes, sizeof(uintptr_t));
    if (code == NULL) {
        return NGX_CONF_ERROR;
    }

    *code = (uintptr_t) NULL;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_rewrite_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_SERVER_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_rewrite_handler;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_rewrite_handler;

    return NGX_OK;
}


/*
 * 解析rewrite指令
 *
 * 从代码中可以看到,只要rewrite指令携带了第三个参数(break、last等)都会结束脚本引擎的执行
 */
static char *
ngx_http_rewrite(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_rewrite_loc_conf_t  *lcf = conf;

    ngx_str_t                         *value;
    ngx_uint_t                         last;
    ngx_regex_compile_t                rc;
    ngx_http_script_code_pt           *code;
    ngx_http_script_compile_t          sc;
    ngx_http_script_regex_code_t      *regex;
    ngx_http_script_regex_end_code_t  *regex_end;
    u_char                             errstr[NGX_MAX_CONF_ERRSTR];

    /*
     * 在数组(lcf->codes)中分配一块sizeof(ngx_http_script_regex_code_t)大小的内存
     */
    regex = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                       sizeof(ngx_http_script_regex_code_t));
    if (regex == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(regex, sizeof(ngx_http_script_regex_code_t));

    value = cf->args->elts;

    ngx_memzero(&rc, sizeof(ngx_regex_compile_t));

    rc.pattern = value[1];
    rc.err.len = NGX_MAX_CONF_ERRSTR;
    rc.err.data = errstr;

    /* TODO: NGX_REGEX_CASELESS */

    regex->regex = ngx_http_regex_compile(cf, &rc);
    if (regex->regex == NULL) {
        return NGX_CONF_ERROR;
    }

    // 脚本引擎要执行的方法
    regex->code = ngx_http_script_regex_start_code;
    //
    regex->uri = 1;
    regex->name = value[1];

    if (value[2].data[value[2].len - 1] == '?') {

        /* the last "?" drops the original arguments */
        value[2].len--;

    } else {
        regex->add_args = 1;
    }

    last = 0;

    if (ngx_strncmp(value[2].data, "http://", sizeof("http://") - 1) == 0
        || ngx_strncmp(value[2].data, "https://", sizeof("https://") - 1) == 0
        || ngx_strncmp(value[2].data, "$scheme", sizeof("$scheme") - 1) == 0)
    {
        regex->status = NGX_HTTP_MOVED_TEMPORARILY;
        regex->redirect = 1;
        last = 1;
    }

    if (cf->args->nelts == 4) {
        if (ngx_strcmp(value[3].data, "last") == 0) {
            /*
             * 向脚本引擎codes中添加终止标志,当脚本引擎检查到该标志后就不会继续向下执行了
             * 但是否会重新走NGX_HTTP_FIND_CONFIG_PHASE阶段重新匹配uri,则看是否有break_cycle标记,有就不走,没有就走
             */
            last = 1;

        } else if (ngx_strcmp(value[3].data, "break") == 0) {
        	// 无视uri的改变
            regex->break_cycle = 1;
            // 向脚本引擎codes中添加终止标志,当脚本引擎检查到该标志后就不会继续向下执行了
            last = 1;

        } else if (ngx_strcmp(value[3].data, "redirect") == 0) {
        	// 302跳转
            regex->status = NGX_HTTP_MOVED_TEMPORARILY;
            regex->redirect = 1;
            last = 1;

        } else if (ngx_strcmp(value[3].data, "permanent") == 0) {
        	// 301跳转
            regex->status = NGX_HTTP_MOVED_PERMANENTLY;
            regex->redirect = 1;
            last = 1;

        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid parameter \"%V\"", &value[3]);
            return NGX_CONF_ERROR;
        }
    }

    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    sc.cf = cf;
    sc.source = &value[2];
    sc.lengths = &regex->lengths;
    sc.values = &lcf->codes;
    sc.variables = ngx_http_script_variables_count(&value[2]);
    sc.main = regex;
    sc.complete_lengths = 1;
    sc.compile_args = !regex->redirect;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    regex = sc.main;

    regex->size = sc.size;
    regex->args = sc.args;

    if (sc.variables == 0 && !sc.dup_capture) {
        regex->lengths = NULL;
    }

    regex_end = ngx_http_script_add_code(lcf->codes,
                                      sizeof(ngx_http_script_regex_end_code_t),
                                      &regex);
    if (regex_end == NULL) {
        return NGX_CONF_ERROR;
    }

    // 				  ngx_http_script_regex_start_code
    regex_end->code = ngx_http_script_regex_end_code;
    regex_end->uri = regex->uri;
    regex_end->args = regex->args;
    regex_end->add_args = regex->add_args;
    regex_end->redirect = regex->redirect;

    if (last) {
        code = ngx_http_script_add_code(lcf->codes, sizeof(uintptr_t), &regex);
        if (code == NULL) {
            return NGX_CONF_ERROR;
        }

        /*
         * 	code
         * 	-----
         * 	| * |
         * 	-----
         *		\
         *		-------------
         *		|   NULL    |
         *		-------------
         * 如果有last标记则在lcf->codes中放一个NULL的ngx_http_script_code_pt()方法,这样脚本引擎就不会往下走了
         * 脚本引擎:
         * 	while (*(uintptr_t *) e->ip) {
         *		code = *(ngx_http_script_code_pt *) e->ip;
         *		code(e);
    	 *	}
    	 * 所以有次可以推断出,该模块所有使用脚本引擎执行的指令就都不会被执行了,比如return指令
    	 *
         */
        *code = NULL;
    }

    regex->next = (u_char *) lcf->codes->elts + lcf->codes->nelts
                                              - (u_char *) regex;

    return NGX_CONF_OK;
}


/*
 * 该方法会把return指令要输出的值放到ngx_http_script_return_code_t对象的text字段中,text字段
 * 是ngx_http_complex_value_t类型的结构体,该结构体用来在运行时计算复杂值(比如 name$uri)
 */
static char *
ngx_http_rewrite_return(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_rewrite_loc_conf_t  *lcf = conf;

    u_char                            *p;
    ngx_str_t                         *value, *v;
    ngx_http_script_return_code_t     *ret;
    ngx_http_compile_complex_value_t   ccv;

    ret = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                     sizeof(ngx_http_script_return_code_t));
    if (ret == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    ngx_memzero(ret, sizeof(ngx_http_script_return_code_t));

    ret->code = ngx_http_script_return_code;

    /*
     * return指令返回的状态码
     */
    p = value[1].data;

    /*
     * 状态码转换为数字
     */
    ret->status = ngx_atoi(p, value[1].len);

    if (ret->status == (uintptr_t) NGX_ERROR) {

        if (cf->args->nelts == 2
            && (ngx_strncmp(p, "http://", sizeof("http://") - 1) == 0
                || ngx_strncmp(p, "https://", sizeof("https://") - 1) == 0
                || ngx_strncmp(p, "$scheme", sizeof("$scheme") - 1) == 0))
        {
            ret->status = NGX_HTTP_MOVED_TEMPORARILY;
            v = &value[1];

        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid return code \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }

    } else {

        if (ret->status > 999) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid return code \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }

        if (cf->args->nelts == 2) {
            return NGX_CONF_OK;
        }

        // return指令中的第三个参数,比如return 200 $http_name
        v = &value[2];
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = v;
    ccv.complex_value = &ret->text;

    /*
     * 编译复杂值
     */
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_rewrite_break(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_rewrite_loc_conf_t *lcf = conf;

    ngx_http_script_code_pt  *code;

    code = ngx_http_script_start_code(cf->pool, &lcf->codes, sizeof(uintptr_t));
    if (code == NULL) {
        return NGX_CONF_ERROR;
    }

    *code = ngx_http_script_break_code;

    return NGX_CONF_OK;
}


static char *
ngx_http_rewrite_if(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	/*
	 * 如果当前指令出现在server{}块中,那么conf就代表rewrite模块在server{}块中对应的ngx_http_rewrite_loc_conf_t结构体
	 * 如果当前指令出现在location{}块中,则conf就代表该模块在location{}块中对应的ngx_http_rewrite_loc_conf_t结构体
	 */
    ngx_http_rewrite_loc_conf_t  *lcf = conf;

    void                         *mconf;
    char                         *rv;
    u_char                       *elts;
    ngx_uint_t                    i;
    ngx_conf_t                    save;
    ngx_http_module_t            *module;
    ngx_http_conf_ctx_t          *ctx, *pctx;
    ngx_http_core_loc_conf_t     *clcf, *pclcf;
    ngx_http_script_if_code_t    *if_code;
    ngx_http_rewrite_loc_conf_t  *nlcf;

    /*
     * 为if () {}块创建一个ngx_http_conf_ctx_t结构体
     */
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    // pctx是当前创建的ctx的父级块拥有的ngx_http_conf_ctx_t结构体,比如server{}和locaiton {}
    pctx = cf->ctx;
    ctx->main_conf = pctx->main_conf;
    ctx->srv_conf = pctx->srv_conf;

    /*
     * 为当前if () {}块分配用于存放所有http模块自定义结构体信息的内存(location级别的)
     */
    ctx->loc_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);
    if (ctx->loc_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = ngx_modules[i]->ctx;

        if (module->create_loc_conf) {

        	// 回调所有http模块的create_loc_conf()方法
            mconf = module->create_loc_conf(cf);
            if (mconf == NULL) {
                 return NGX_CONF_ERROR;
            }

            ctx->loc_conf[ngx_modules[i]->ctx_index] = mconf;
        }
    }

    /*
     * pclcf表示if () {}块所在的上级块
     *
     * 如果上级是server{}块,那么pclcf就表示http核心模块在server{}块中的ngx_http_core_loc_conf_t结构体信息,
     * 该结构体locations字段包含了在该块下的所有loction{}块,比如:
     * 		server {
     * 			if ($uri) {
     * 				// something
     * 			}
     *
     * 			location /uri {
     * 				// something
     * 			}
     * 		}
     *
     * 如果上级是location{}块,那么pclcf就表示http模块在location{}块中的ngx_http_core_loc_conf_t结构体信息,
     * 结构体locations字段包含了在该块下的所有loction{}块,比如:
     * 		location {
     * 			if ($uri) {
     * 				// something
     * 			}
     *
     * 			location /uri {
     * 				// something
     * 			}
     * 		}
     *
     */
    pclcf = pctx->loc_conf[ngx_http_core_module.ctx_index];

    // 取出代表if () {}块本身的ngx_http_core_loc_conf_t结构体
    clcf = ctx->loc_conf[ngx_http_core_module.ctx_index];
    clcf->loc_conf = ctx->loc_conf;
    clcf->name = pclcf->name;
    // 标记为匿名location
    clcf->noname = 1;

    /*
     * 把代表自己的ngx_http_core_loc_conf_t结构体放到父块的locations字段中
     *
     * TODO 目前还没有看明白加入这里的目的,因为if的执行不需要在NGX_HTTP_FIND_CONFIG_PHASE阶段匹配locaiton。
     * 所以按理说这没必要把该结构体放入到locations队列中。虽然@类型的location也不在FIND阶段进行匹配,但@类型的
     * location会在ngx_http_init_locations()方法中将其放入到ngx_http_core_srv_conf_t.named_locatons字段
     * 中,所以@类型的locaiton需要放入到locations队列中,但目前if类型的会在ngx_http_init_locations()方法中丢弃,
     * 难道是为了以后的扩展,或者代码风格上保持一致,或者我还没有看到使用的地方?
     */
    if (ngx_http_add_location(cf, &pclcf->locations, clcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (ngx_http_rewrite_if_condition(cf, lcf) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }

    if_code = ngx_array_push_n(lcf->codes, sizeof(ngx_http_script_if_code_t));
    if (if_code == NULL) {
        return NGX_CONF_ERROR;
    }

    if_code->code = ngx_http_script_if_code;

    elts = lcf->codes->elts;


    /* the inner directives must be compiled to the same code array */
    /*
     * 把if () {}内的指令操作都编译到父级的codes数组中,这样在执行if () {}的时候就都是通过脚本引擎来执行了,
     * 所以出现在if () {}内的变量会和它的父级变量在一个codes数组中执行。
     * 对于如下配置:
     * 		locationg / {
     *			proxy_pass	http://www.baidu.com;
     *
	 * 			if ($uri) {
	 * 				proxy_pass http://www.jd.com;
	 * 			}
     * 		}
     * 我们知道在ngx中if本身会被看成一个location对待,所以它也有对应的ngx_http_core_loc_conf_t结构体,然而location的
     * 匹配结果就是拿到对应的ngx_http_core_loc_conf_t结构体,然后后续的各个阶段中运行的loc级别的指令信息,都会从该结构体中
     * 索引到,所以上面的if执行成功也就表示拿到了对应的ngx_http_core_loc_conf_t结构体,所以就正确的执行了匹配的proxy_pass指令。
     */
    nlcf = ctx->loc_conf[ngx_http_rewrite_module.ctx_index];
    /*
     * 父级codes数组赋值给代表当前if的codes数组,这样在调用ngx_conf_parse方法进行解析的时候,会很方便的将脚本
     * 代码放入到父级codes数组中了
     */
    nlcf->codes = lcf->codes;


    save = *cf;
    cf->ctx = ctx;

    if (pclcf->name.len == 0) {
        if_code->loc_conf = NULL;
        // 在server{}中的if
        cf->cmd_type = NGX_HTTP_SIF_CONF;

    } else {
        if_code->loc_conf = ctx->loc_conf;
        // 在locatoin{}中的if
        cf->cmd_type = NGX_HTTP_LIF_CONF;
    }

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    if (rv != NGX_CONF_OK) {
        return rv;
    }


    if (elts != lcf->codes->elts) {
        if_code = (ngx_http_script_if_code_t *)
                   ((u_char *) if_code + ((u_char *) lcf->codes->elts - elts));
    }

    if_code->next = (u_char *) lcf->codes->elts + lcf->codes->nelts
                                                - (u_char *) if_code;

    /* the code array belong to parent block */
    // 属于它的codes已经放入到父块的codes中,这里直接把这个变量置空
    nlcf->codes = NULL;

    return NGX_CONF_OK;
}


static char *
ngx_http_rewrite_if_condition(ngx_conf_t *cf, ngx_http_rewrite_loc_conf_t *lcf)
{
    u_char                        *p;
    size_t                         len;
    ngx_str_t                     *value;
    ngx_uint_t                     cur, last;
    ngx_regex_compile_t            rc;
    ngx_http_script_code_pt       *code;
    ngx_http_script_file_code_t   *fop;
    ngx_http_script_regex_code_t  *regex;
    u_char                         errstr[NGX_MAX_CONF_ERRSTR];

    value = cf->args->elts;
    last = cf->args->nelts - 1;

    if (value[1].len < 1 || value[1].data[0] != '(') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid condition \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (value[1].len == 1) {
        cur = 2;

    } else {
        cur = 1;
        value[1].len--;
        value[1].data++;
    }

    if (value[last].len < 1 || value[last].data[value[last].len - 1] != ')') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid condition \"%V\"", &value[last]);
        return NGX_CONF_ERROR;
    }

    if (value[last].len == 1) {
        last--;

    } else {
        value[last].len--;
        value[last].data[value[last].len] = '\0';
    }

    len = value[cur].len;
    p = value[cur].data;

    if (len > 1 && p[0] == '$') {

        if (cur != last && cur + 2 != last) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid condition \"%V\"", &value[cur]);
            return NGX_CONF_ERROR;
        }

        if (ngx_http_rewrite_variable(cf, lcf, &value[cur]) != NGX_CONF_OK) {
            return NGX_CONF_ERROR;
        }

        if (cur == last) {
            return NGX_CONF_OK;
        }

        cur++;

        len = value[cur].len;
        p = value[cur].data;

        if (len == 1 && p[0] == '=') {

            if (ngx_http_rewrite_value(cf, lcf, &value[last]) != NGX_CONF_OK) {
                return NGX_CONF_ERROR;
            }

            code = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                              sizeof(uintptr_t));
            if (code == NULL) {
                return NGX_CONF_ERROR;
            }

            *code = ngx_http_script_equal_code;

            return NGX_CONF_OK;
        }

        if (len == 2 && p[0] == '!' && p[1] == '=') {

            if (ngx_http_rewrite_value(cf, lcf, &value[last]) != NGX_CONF_OK) {
                return NGX_CONF_ERROR;
            }

            code = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                              sizeof(uintptr_t));
            if (code == NULL) {
                return NGX_CONF_ERROR;
            }

            *code = ngx_http_script_not_equal_code;
            return NGX_CONF_OK;
        }

        if ((len == 1 && p[0] == '~')
            || (len == 2 && p[0] == '~' && p[1] == '*')
            || (len == 2 && p[0] == '!' && p[1] == '~')
            || (len == 3 && p[0] == '!' && p[1] == '~' && p[2] == '*'))
        {
            regex = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                         sizeof(ngx_http_script_regex_code_t));
            if (regex == NULL) {
                return NGX_CONF_ERROR;
            }

            ngx_memzero(regex, sizeof(ngx_http_script_regex_code_t));

            ngx_memzero(&rc, sizeof(ngx_regex_compile_t));

            rc.pattern = value[last];
            rc.options = (p[len - 1] == '*') ? NGX_REGEX_CASELESS : 0;
            rc.err.len = NGX_MAX_CONF_ERRSTR;
            rc.err.data = errstr;

            regex->regex = ngx_http_regex_compile(cf, &rc);
            if (regex->regex == NULL) {
                return NGX_CONF_ERROR;
            }

            regex->code = ngx_http_script_regex_start_code;
            regex->next = sizeof(ngx_http_script_regex_code_t);
            regex->test = 1;
            if (p[0] == '!') {
                regex->negative_test = 1;
            }
            regex->name = value[last];

            return NGX_CONF_OK;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unexpected \"%V\" in condition", &value[cur]);
        return NGX_CONF_ERROR;

    } else if ((len == 2 && p[0] == '-')
               || (len == 3 && p[0] == '!' && p[1] == '-'))
    {
        if (cur + 1 != last) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid condition \"%V\"", &value[cur]);
            return NGX_CONF_ERROR;
        }

        value[last].data[value[last].len] = '\0';
        value[last].len++;

        if (ngx_http_rewrite_value(cf, lcf, &value[last]) != NGX_CONF_OK) {
            return NGX_CONF_ERROR;
        }

        fop = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                          sizeof(ngx_http_script_file_code_t));
        if (fop == NULL) {
            return NGX_CONF_ERROR;
        }

        fop->code = ngx_http_script_file_code;

        if (p[1] == 'f') {
            fop->op = ngx_http_script_file_plain;
            return NGX_CONF_OK;
        }

        if (p[1] == 'd') {
            fop->op = ngx_http_script_file_dir;
            return NGX_CONF_OK;
        }

        if (p[1] == 'e') {
            fop->op = ngx_http_script_file_exists;
            return NGX_CONF_OK;
        }

        if (p[1] == 'x') {
            fop->op = ngx_http_script_file_exec;
            return NGX_CONF_OK;
        }

        if (p[0] == '!') {
            if (p[2] == 'f') {
                fop->op = ngx_http_script_file_not_plain;
                return NGX_CONF_OK;
            }

            if (p[2] == 'd') {
                fop->op = ngx_http_script_file_not_dir;
                return NGX_CONF_OK;
            }

            if (p[2] == 'e') {
                fop->op = ngx_http_script_file_not_exists;
                return NGX_CONF_OK;
            }

            if (p[2] == 'x') {
                fop->op = ngx_http_script_file_not_exec;
                return NGX_CONF_OK;
            }
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid condition \"%V\"", &value[cur]);
        return NGX_CONF_ERROR;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid condition \"%V\"", &value[cur]);

    return NGX_CONF_ERROR;
}


static char *
ngx_http_rewrite_variable(ngx_conf_t *cf, ngx_http_rewrite_loc_conf_t *lcf,
    ngx_str_t *value)
{
    ngx_int_t                    index;
    ngx_http_script_var_code_t  *var_code;

    value->len--;
    value->data++;

    index = ngx_http_get_variable_index(cf, value);

    if (index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    var_code = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                          sizeof(ngx_http_script_var_code_t));
    if (var_code == NULL) {
        return NGX_CONF_ERROR;
    }

    var_code->code = ngx_http_script_var_code;
    var_code->index = index;

    return NGX_CONF_OK;
}


/*
 * 设置变量的引擎数据
 *
 * 主要是设置lcf->codes字段,这里面存放了脚本引擎执行时需要的数据:
 * 	每个变量值有两个结构体来表示,ngx_http_script_value_code_t和ngx_http_script_var_code_t
 * 	第一个用来保存变量值,第二个用来保存变量在cmcf->variables中的下标
 *
 * 	第一个结构体中的ngx_http_script_value_code()方法将变量值压入栈(ngx_http_script_engine_t->sp++)
 * 	第二个结构体中的ngx_http_script_set_var_code()方法将变量值从栈(ngx_http_script_engine_t->sp--)
 * 		取出,并对r->variables中对应的变量赋值
 *
 * 最终引擎有ngx_http_rewrite_handler()方法执行
 */
static char *
ngx_http_rewrite_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_rewrite_loc_conf_t  *lcf = conf;

    ngx_int_t                            index;
    ngx_str_t                           *value;
    ngx_http_variable_t                 *v;
    ngx_http_script_var_code_t          *vcode;
    ngx_http_script_var_handler_code_t  *vhcode;

    value = cf->args->elts;

    if (value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    value[1].len--;
    value[1].data++;

    /*
     * 将变量名字添加到cmcf->variables_keys数组中
     */
    v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * 将变量的名字添加到cmcf->variables数组中,并返回变量在数组中的下标
     */
    index = ngx_http_get_variable_index(cf, &value[1]);
    if (index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    if (v->get_handler == NULL
        && ngx_strncasecmp(value[1].data, (u_char *) "http_", 5) != 0
        && ngx_strncasecmp(value[1].data, (u_char *) "sent_http_", 10) != 0
        && ngx_strncasecmp(value[1].data, (u_char *) "upstream_http_", 14) != 0
        && ngx_strncasecmp(value[1].data, (u_char *) "cookie_", 7) != 0
        && ngx_strncasecmp(value[1].data, (u_char *) "upstream_cookie_", 16)
           != 0
        && ngx_strncasecmp(value[1].data, (u_char *) "arg_", 4) != 0)
    {
    	/*
    	 * 自定义变量会走这个逻辑,比如:
    	 * 		set $a aaa;
    	 * 为变量设置一个默认取值方法,当ngx中使用了未初始化的变量的时候会调用这个方法,比如下面的配置
    	 *     location / {
    	 *         set $b "$a";
    	 *         set $a "hello";
    	 *         return 200 "$b";
    	 *     }
    	 * 当访问该location的时候返回值为空字符串(""),并且后台日志会打印一个警告值
    	 *     using uninitialized "a" variable
    	 * 执行过程是这样的,上面的三个指令会按照配置顺序依次执行
    	 * 1.执行第一个set指令的时候通过ngx_http_get_flushed_variable()方法去获取变量"$a"的值,
    	 *   由于是第一使用,所以在请求的r->variables中是没有该变量缓存值的,所以需要调用变量"$a"的get_handler()
    	 *   方法获取该变量值,该变量的get_handler()就是ngx_http_rewrite_var()方法,这个返回会打印一个警告,并
    	 *   返回空字符("").
    	 * 2.执行变量"$b"对应的脚本方法,把脚本引擎中的空字符赋值给该变量
    	 * 3.执行第二set指令的脚本code,步骤同1和2
    	 * 4.执行return对应的脚本code,然后返回变量"$b"的值,也就是空字符
    	 */
        v->get_handler = ngx_http_rewrite_var;
        v->data = index;

    }

    /*
     * 把变量值(ngx_http_script_value_code_t)放入到lcf->codes中
     *
     * 执行完该代码后lcf->codes数组中存放的就是ngx_http_script_value_code_t
     *
     * 该方法还会处理复杂变量值(set $a $bb$cc)
     *
     * 这个方法会把
     *    set $a $bb$cc
     * 中的复杂值编译到lcf->codes中
     */
    if (ngx_http_rewrite_value(cf, lcf, &value[2]) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }

    /*
     * 下面的逻辑会把
     *    set $a $bb$cc
     * 中定义的变量
     *    $a
     * 对应的执行脚本放到lcf->codes中,对应的脚本会执行ngx_http_script_var_set_handler_code()方法
     * 或者ngx_http_script_set_var_code()方法,这两个方法都是在设置变量($a)的值,他们会用到上一个脚本生成的值,
     * 上一个脚本生成的值会放到脚本引擎e->sp中,比如ngx_http_script_value_code()方法或者
     * ngx_http_script_complex_value_code()方法.
     */

    if (v->set_handler) {
        vhcode = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                   sizeof(ngx_http_script_var_handler_code_t));
        if (vhcode == NULL) {
            return NGX_CONF_ERROR;
        }

        vhcode->code = ngx_http_script_var_set_handler_code;
        vhcode->handler = v->set_handler;
        vhcode->data = v->data;

        return NGX_CONF_OK;
    }

    /*
     * 把变量(ngx_http_script_var_code_t)放入到lcf->codes中
     *
     * 从这里可以看到对于指令 set $a $bb$cc,ngx会先把变量值放入到脚本数组中(lcf->codes),
     * 然后把变量放入到脚本数组中,这样ngx脚本引擎在执行的时候就会先获取变量值,然后在给变量赋值了
     *
     * 也就是说,在脚本引擎设计中
     *  ngx_http_script_value_code_t结构体负责存储脚本值信息
     *  ngx_http_script_var_code_t结构体负责存储处理脚本信息的方法
     *
     * 对于这两个结构体,如果从他们携带的方法角度描述的话可以这两样理解,第一个结构体的方法负责取变量值,
     * 第二个结构体的方法负责为变量赋值。
     */
    vcode = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                       sizeof(ngx_http_script_var_code_t));
    if (vcode == NULL) {
        return NGX_CONF_ERROR;
    }

    // 设置为变量赋值的方法
    vcode->code = ngx_http_script_set_var_code;
    vcode->index = (uintptr_t) index;

    return NGX_CONF_OK;
}


/**
 * 将变量值放入到lcf->codes中
 *
 * 对于复杂变量值使用ngx_http_script_complex_value_code_t来处理
 *
 * TODO 如果value等于 $http_xxx该如何处理
 */
static char *
ngx_http_rewrite_value(ngx_conf_t *cf, ngx_http_rewrite_loc_conf_t *lcf,
    ngx_str_t *value)
{
    ngx_int_t                              n;
    ngx_http_script_compile_t              sc;
    ngx_http_script_value_code_t          *val;
    ngx_http_script_complex_value_code_t  *complex;

    n = ngx_http_script_variables_count(value);

    if (n == 0) {
    	/* 走这个逻辑说明变量值中没有其他变量 */

        val = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                         sizeof(ngx_http_script_value_code_t));
        if (val == NULL) {
            return NGX_CONF_ERROR;
        }

        n = ngx_atoi(value->data, value->len);

        if (n == NGX_ERROR) {
            n = 0;
        }

        val->code = ngx_http_script_value_code;
        val->value = (uintptr_t) n;
        val->text_len = (uintptr_t) value->len;
        // 变量值放入到val->text_data中
        val->text_data = (uintptr_t) value->data;

        return NGX_CONF_OK;
    }

    /*
     * 走到这里说明变量值中存在其它变量,比如:
     * 	set $a $bb$cc$dd
     *
     * 该函数的作用是向lcf->codes数组中添加ngx_http_script_complex_value_code_t结构体
     */
    complex = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                 sizeof(ngx_http_script_complex_value_code_t));
    if (complex == NULL) {
        return NGX_CONF_ERROR;
    }

    complex->code = ngx_http_script_complex_value_code;
    complex->lengths = NULL;

    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    sc.cf = cf;
    // 复杂变量值,如 set $a $bb 中的$bb
    sc.source = value;
    sc.lengths = &complex->lengths;
    sc.values = &lcf->codes;
    // 复杂变量值中,变量个数
    sc.variables = n;
    sc.complete_lengths = 1;

    // 解析变量值中的变量
    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
