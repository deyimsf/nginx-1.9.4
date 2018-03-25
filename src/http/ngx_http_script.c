
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static ngx_int_t ngx_http_script_init_arrays(ngx_http_script_compile_t *sc);
static ngx_int_t ngx_http_script_done(ngx_http_script_compile_t *sc);
static ngx_int_t ngx_http_script_add_copy_code(ngx_http_script_compile_t *sc,
    ngx_str_t *value, ngx_uint_t last);
static ngx_int_t ngx_http_script_add_var_code(ngx_http_script_compile_t *sc,
    ngx_str_t *name);
static ngx_int_t ngx_http_script_add_args_code(ngx_http_script_compile_t *sc);
#if (NGX_PCRE)
static ngx_int_t ngx_http_script_add_capture_code(ngx_http_script_compile_t *sc,
     ngx_uint_t n);
#endif
static ngx_int_t
     ngx_http_script_add_full_name_code(ngx_http_script_compile_t *sc);
static size_t ngx_http_script_full_name_len_code(ngx_http_script_engine_t *e);
static void ngx_http_script_full_name_code(ngx_http_script_engine_t *e);


#define ngx_http_script_exit  (u_char *) &ngx_http_script_exit_code

static uintptr_t ngx_http_script_exit_code = (uintptr_t) NULL;


void
ngx_http_script_flush_complex_value(ngx_http_request_t *r,
    ngx_http_complex_value_t *val)
{
    ngx_uint_t *index;

    index = val->flushes;

    if (index) {
        while (*index != (ngx_uint_t) -1) {

            if (r->variables[*index].no_cacheable) {
                r->variables[*index].valid = 0;
                r->variables[*index].not_found = 0;
            }

            index++;
        }
    }
}


/*
 * 计算出变量值
 *
 * 通过r和val计算出val代表的表达式的值,并写到value中
 */
ngx_int_t
ngx_http_complex_value(ngx_http_request_t *r, ngx_http_complex_value_t *val,
    ngx_str_t *value)
{
    size_t                        len;
    ngx_http_script_code_pt       code;
    ngx_http_script_len_code_pt   lcode;
    ngx_http_script_engine_t      e;

    if (val->lengths == NULL) {
    	/*
    	 * 这里使用val->lengths字段是NULL来判断要获取的复杂值是一个纯文本值,所以直接返回这个纯文本值就可以了
    	 *
    	 * 比如如下语句:
    	 *    return 200 "I am value";
    	 * 在ngx_http_rewrite_return()方法中通过调用ngx_http_compile_complex_value()方法来编译这个
    	 * 复杂值,编译的过程中发现该值只是一个纯文本值,所以他仅会设置val->value值,其它值如val->lengths设置
    	 * 为NULL,所以这里可以用val->lengths等于NULL来区分该复杂值是否是纯文本值.
    	 */

        *value = val->value;
        return NGX_OK;
    }

    ngx_http_script_flush_complex_value(r, val);

    ngx_memzero(&e, sizeof(ngx_http_script_engine_t));

    e.ip = val->lengths;
    e.request = r;
    e.flushed = 1;

    len = 0;

    while (*(uintptr_t *) e.ip) {
        lcode = *(ngx_http_script_len_code_pt *) e.ip;
        len += lcode(&e);
    }

    value->len = len;
    value->data = ngx_pnalloc(r->pool, len);
    if (value->data == NULL) {
        return NGX_ERROR;
    }

    e.ip = val->values;
    e.pos = value->data;
    e.buf = *value;

    while (*(uintptr_t *) e.ip) {
        code = *(ngx_http_script_code_pt *) e.ip;
        code((ngx_http_script_engine_t *) &e);
    }

    *value = e.buf;

    return NGX_OK;
}


/*
 * 编译收集到的复杂值,把编译后的数据放入到ngx_http_complex_value_t结构体中(ccv->complex_value字段)
 */
ngx_int_t
ngx_http_compile_complex_value(ngx_http_compile_complex_value_t *ccv)
{
    ngx_str_t                  *v;
    ngx_uint_t                  i, n, nv, nc;
    ngx_array_t                 flushes, lengths, values, *pf, *pl, *pv;
    ngx_http_script_compile_t   sc;

    v = ccv->value;

    /*
     * 复杂值v包含的变量值的个数,比如有如下复杂值
     *   "I am $uri $host"
     * 那么nv值为2,表示在v中有两个变量值
     */
    nv = 0;
    /*
     * 复杂值v中包含正则表达式子捕获的个数,比如有如下复杂值
     *   "I am $1 $2 $3"
     * 那么nc值为3,表示在v中有3个有效的子捕获变量值
     */
    nc = 0;

    for (i = 0; i < v->len; i++) {
        if (v->data[i] == '$') {
            if (v->data[i + 1] >= '1' && v->data[i + 1] <= '9') {
                nc++;

            } else {
                nv++;
            }
        }
    }

    if ((v->len == 0 || v->data[0] != '$')
        && (ccv->conf_prefix || ccv->root_prefix))
    {
        if (ngx_conf_full_name(ccv->cf->cycle, v, ccv->conf_prefix) != NGX_OK) {
            return NGX_ERROR;
        }

        ccv->conf_prefix = 0;
        ccv->root_prefix = 0;
    }

    ccv->complex_value->value = *v;
    ccv->complex_value->flushes = NULL;
    ccv->complex_value->lengths = NULL;
    ccv->complex_value->values = NULL;

    if (nv == 0 && nc == 0) {
    	/*
    	 * 如果这两个值都是0,表示ccv->value是一个纯文本值
    	 * 如果是纯文本,则flushes、lengths、values三个字段为NULL
    	 */
        return NGX_OK;
    }

    n = nv + 1;

    /*
     * 走道这里说明复杂值中包含变量,比如
     *    reutrn 200 "I am $uri";
     *
     * flushes里面存放了n个元素,每个元素占sizeof(ngx_uint_t)个字节,假设n是3,则相当于存放了3个整数值
     *
     * flushes
     * ----------------------------------------------------------------
     * | sizeof(ngx_uint_t) | sizeof(ngx_uint_t) | sizeof(ngx_uint_t) |
     * ----------------------------------------------------------------
     */
    if (ngx_array_init(&flushes, ccv->cf->pool, n, sizeof(ngx_uint_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    n = nv * (2 * sizeof(ngx_http_script_copy_code_t)
                  + sizeof(ngx_http_script_var_code_t))
                		 + sizeof(uintptr_t);

    /*
     *
     */
    if (ngx_array_init(&lengths, ccv->cf->pool, n, 1) != NGX_OK) {
        return NGX_ERROR;
    }

    n = (nv * (2 * sizeof(ngx_http_script_copy_code_t)
                   + sizeof(ngx_http_script_var_code_t))
    		       + sizeof(uintptr_t) + v->len
                   + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);

    if (ngx_array_init(&values, ccv->cf->pool, n, 1) != NGX_OK) {
        return NGX_ERROR;
    }

    pf = &flushes;
    pl = &lengths;
    pv = &values;

    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    /*
     * 下面代码赋值完毕后pf内存结构如下:
     *         sc.flushes
     *           -----
     *           | * |
     *           -----
     *              \  pf
     *               -----
     *               | * |
     *               -----
     *                /
     *           -----------
     *           | flushes |
     *           -----------
     */
    sc.cf = ccv->cf;
    sc.source = v;
    sc.flushes = &pf;
    sc.lengths = &pl;
    sc.values = &pv;
    sc.complete_lengths = 1;
    sc.complete_values = 1;
    sc.zero = ccv->zero;
    sc.conf_prefix = ccv->conf_prefix;
    sc.root_prefix = ccv->root_prefix;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_ERROR;
    }

    if (flushes.nelts) {
        ccv->complex_value->flushes = flushes.elts;

        /* 最后一个设置为-1作为结束标记 */
        ccv->complex_value->flushes[flushes.nelts] = (ngx_uint_t) -1;
    }

    ccv->complex_value->lengths = lengths.elts;
    ccv->complex_value->values = values.elts;

    return NGX_OK;
}


char *
ngx_http_set_complex_value_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_str_t                          *value;
    ngx_http_complex_value_t          **cv;
    ngx_http_compile_complex_value_t    ccv;

    cv = (ngx_http_complex_value_t **) (p + cmd->offset);

    if (*cv != NULL) {
        return "duplicate";
    }

    *cv = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
    if (*cv == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = *cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


ngx_int_t
ngx_http_test_predicates(ngx_http_request_t *r, ngx_array_t *predicates)
{
    ngx_str_t                  val;
    ngx_uint_t                 i;
    ngx_http_complex_value_t  *cv;

    if (predicates == NULL) {
        return NGX_OK;
    }

    cv = predicates->elts;

    for (i = 0; i < predicates->nelts; i++) {
        if (ngx_http_complex_value(r, &cv[i], &val) != NGX_OK) {
            return NGX_ERROR;
        }

        if (val.len && (val.len != 1 || val.data[0] != '0')) {
            return NGX_DECLINED;
        }
    }

    return NGX_OK;
}


char *
ngx_http_set_predicate_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_str_t                          *value;
    ngx_uint_t                          i;
    ngx_array_t                       **a;
    ngx_http_complex_value_t           *cv;
    ngx_http_compile_complex_value_t    ccv;

    a = (ngx_array_t **) (p + cmd->offset);

    if (*a == NGX_CONF_UNSET_PTR) {
        *a = ngx_array_create(cf->pool, 1, sizeof(ngx_http_complex_value_t));
        if (*a == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        cv = ngx_array_push(*a);
        if (cv == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

        ccv.cf = cf;
        ccv.value = &value[i];
        ccv.complex_value = cv;

        if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


/*
 * 计算value中变量的个数,也就是$的个数
 */
ngx_uint_t
ngx_http_script_variables_count(ngx_str_t *value)
{
    ngx_uint_t  i, n;

    for (n = 0, i = 0; i < value->len; i++) {
        if (value->data[i] == '$') {
            n++;
        }
    }

    return n;
}


/*
 * TODO 没看懂,以后有时间再看
 *
 * 将变量(如$http_name、$age等)放入cmcv->variabls中并存储变量在数组中的下标
 *
 *
 * 在ngx_http_compile_complex_value()方法中有如下调用
 *   sc.cf = ccv->cf;
 *   sc.source = v;
 *   sc.flushes = &pf;
 *   sc.lengths = &pl;
 *   sc.values = &pv;
 *   sc.complete_lengths = 1;
 *   sc.complete_values = 1;
 *   sc.zero = ccv->zero;
 *   sc.conf_prefix = ccv->conf_prefix;
 *   sc.root_prefix = ccv->root_prefix;
 */
ngx_int_t
ngx_http_script_compile(ngx_http_script_compile_t *sc)
{
    u_char       ch;
    ngx_str_t    name;
    ngx_uint_t   i, bracket; /* bracket 当前变量是否包含花括号 */

    /*
     * 初始化sc(ngx_http_script_compile_t)
     *
     * 主要是为*sc->flushes、*sc->lengths、*sc->values分配内存空间
     *    sc->flushes
     *      -----
     *      | * |
     *      -----
     *        \ pf (ngx_http_compile_complex_value方法中的变量)
     *        -----
     *        | * |
     *        -----
     *          /
     *      -----------
     *      | flushes |
     *      -----------
     */
    if (ngx_http_script_init_arrays(sc) != NGX_OK) {
        return NGX_ERROR;
    }

    /*
     * 遍历复杂值,假设此时在编译如下指令的复杂值：
     *    return 200 "I am $uri";
     * 则此时source代表"I am $uri"字符串
     *
     */
    for (i = 0; i < sc->source->len; /* void */ ) {

        name.len = 0;

        if (sc->source->data[i] == '$') {

        	/*
        	 * 走到这里说明可能找到一个变量
        	 */

            if (++i == sc->source->len) {
            	/*
            	 * 如果当前变量值最后一个字符是$则该变量值无效
            	 */
                goto invalid_variable;
            }

            // 判断是否使用了正则表达式中的捕获形式,如$1、$2、$3等
#if (NGX_PCRE)
            {
            ngx_uint_t  n;

            if (sc->source->data[i] >= '1' && sc->source->data[i] <= '9') {

                n = sc->source->data[i] - '0';

                if (sc->captures_mask & (1 << n)) {
                    sc->dup_capture = 1;
                }

                sc->captures_mask |= 1 << n;

                if (ngx_http_script_add_capture_code(sc, n) != NGX_OK) {
                    return NGX_ERROR;
                }

                i++;

                continue;
            }
            }
#endif

            /*
             * 如果紧跟'$'字符之后是一个'{'字符,比如
             *    return 200 "I am ${uri}"
             */
            if (sc->source->data[i] == '{') {
            	// 记录当前正在遍历括号('{}')内字符
                bracket = 1;

                if (++i == sc->source->len) {
                    goto invalid_variable;
                }

                // 记录去掉变量前缀的字符指针值
                name.data = &sc->source->data[i];

            } else {
            	/*
            	 * 当前变量不包含括号('{}')
            	 */
                bracket = 0;

                /*
                 * 记录去掉变量前缀的字符指针值,比如
                 *    "$uri"
                 * 则此时name.data指向
                 *    "uri"
                 */
                name.data = &sc->source->data[i];
            }

            /*
             * 计算出不带'$'符号的变量值,比如
             *    "$uri"和"${uri}"
             * 目的是找出
             *    "uri"
             * 也就是说去掉了"$"和"{}"字符
             */
            for ( /* void */ ; i < sc->source->len; i++, name.len++) {
                ch = sc->source->data[i];

                if (ch == '}' && bracket) {
                    i++;
                    bracket = 0;

                    /*
                     * 走到这里说明找出了一个带花括号的变量值,比如
                     *   "${uri}"
                     */
                    break;
                }

                if ((ch >= 'A' && ch <= 'Z')
                    || (ch >= 'a' && ch <= 'z')
                    || (ch >= '0' && ch <= '9')
                    || ch == '_')
                {
                    continue;
                }

                /*
                 * 走到这里说明找出了一个不带话括号的变量值,比如
                 *    "$uri"
                 */
                break;
            }

            if (bracket) {
                ngx_conf_log_error(NGX_LOG_EMERG, sc->cf, 0,
                                   "the closing bracket in \"%V\" "
                                   "variable is missing", &name);
                return NGX_ERROR;
            }

            if (name.len == 0) {
                goto invalid_variable;
            }

            /*
             * 变量值中变量的个数加一
             */
            sc->variables++;

            /*
             * 添加一个计算变量的脚本
             */
            if (ngx_http_script_add_var_code(sc, &name) != NGX_OK) {
                return NGX_ERROR;
            }

            continue;
        }

        if (sc->source->data[i] == '?' && sc->compile_args) {
            sc->args = 1;
            sc->compile_args = 0;

            if (ngx_http_script_add_args_code(sc) != NGX_OK) {
                return NGX_ERROR;
            }

            i++;

            continue;
        }

        name.data = &sc->source->data[i];
        /*
         * 下面这个循环用来跳出复杂值的中的字面量,比如
         *    return 200 "I am $uri";
         * 则该循环会把name.len设置为5,不包括'$'符号,遇见'$'符号后表示可能找到一个变量,需要跳到
         * 最开始的if去解析出这个变量
         */
        while (i < sc->source->len) {

            if (sc->source->data[i] == '$') {
                break;
            }

            if (sc->source->data[i] == '?') {

                sc->args = 1;

                if (sc->compile_args) {
                    break;
                }
            }

            i++;
            name.len++;
        }

        sc->size += name.len;

        /*
         * sc.cf = ccv->cf;
         * sc.source = v;
         * sc.flushes = &pf;
         * sc.lengths = &pl;
         * sc.values = &pv;
         * sc.complete_lengths = 1;
         * sc.complete_values = 1;
         * sc.zero = ccv->zero;
         * sc.conf_prefix = ccv->conf_prefix;
         * sc.root_prefix = ccv->root_prefix;
         */
        if (ngx_http_script_add_copy_code(sc, &name, (i == sc->source->len))
            != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    return ngx_http_script_done(sc);

invalid_variable:

    ngx_conf_log_error(NGX_LOG_EMERG, sc->cf, 0, "invalid variable name");

    return NGX_ERROR;
}


u_char *
ngx_http_script_run(ngx_http_request_t *r, ngx_str_t *value,
    void *code_lengths, size_t len, void *code_values)
{
    ngx_uint_t                    i;
    ngx_http_script_code_pt       code;
    ngx_http_script_len_code_pt   lcode;
    ngx_http_script_engine_t      e;
    ngx_http_core_main_conf_t    *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);

    for (i = 0; i < cmcf->variables.nelts; i++) {
        if (r->variables[i].no_cacheable) {
            r->variables[i].valid = 0;
            r->variables[i].not_found = 0;
        }
    }

    ngx_memzero(&e, sizeof(ngx_http_script_engine_t));

    e.ip = code_lengths;
    e.request = r;
    e.flushed = 1;

    while (*(uintptr_t *) e.ip) {
        lcode = *(ngx_http_script_len_code_pt *) e.ip;
        len += lcode(&e);
    }


    value->len = len;
    value->data = ngx_pnalloc(r->pool, len);
    if (value->data == NULL) {
        return NULL;
    }

    e.ip = code_values;
    e.pos = value->data;

    while (*(uintptr_t *) e.ip) {
        code = *(ngx_http_script_code_pt *) e.ip;
        code((ngx_http_script_engine_t *) &e);
    }

    return e.pos;
}


void
ngx_http_script_flush_no_cacheable_variables(ngx_http_request_t *r,
    ngx_array_t *indices)
{
    ngx_uint_t  n, *index;

    if (indices) {
        index = indices->elts;
        for (n = 0; n < indices->nelts; n++) {
            if (r->variables[index[n]].no_cacheable) {
                r->variables[index[n]].valid = 0;
                r->variables[index[n]].not_found = 0;
            }
        }
    }
}


/*
 * 初始化*sc->flushes
 */
static ngx_int_t
ngx_http_script_init_arrays(ngx_http_script_compile_t *sc)
{
    ngx_uint_t   n;

    if (sc->flushes && *sc->flushes == NULL) {
        n = sc->variables ? sc->variables : 1;
        /*
         * 	sc->flushes
         * 	 -------
         * 	 |  *  |
         * 	 -------
         * 	    \ pf
         * 	    -------
         * 	    |  *  |
         * 	    -------
         * 	    	\
         * 	    	---------------
         * 	    	| ngx_array_t |
         * 	    	---------------
         * 	    			\ elts
         * 	    			----------
         * 	    			|  |  |  |
         * 	    			----------
         */
        *sc->flushes = ngx_array_create(sc->cf->pool, n, sizeof(ngx_uint_t));
        if (*sc->flushes == NULL) {
            return NGX_ERROR;
        }
    }

    if (*sc->lengths == NULL) {
        n = sc->variables * (2 * sizeof(ngx_http_script_copy_code_t)
                             + sizeof(ngx_http_script_var_code_t))
            + sizeof(uintptr_t);

        *sc->lengths = ngx_array_create(sc->cf->pool, n, 1);
        if (*sc->lengths == NULL) {
            return NGX_ERROR;
        }
    }

    if (*sc->values == NULL) {
        n = (sc->variables * (2 * sizeof(ngx_http_script_copy_code_t)
                              + sizeof(ngx_http_script_var_code_t))
                + sizeof(uintptr_t)
                + sc->source->len
                + sizeof(uintptr_t) - 1)
            & ~(sizeof(uintptr_t) - 1);

        *sc->values = ngx_array_create(sc->cf->pool, n, 1);
        if (*sc->values == NULL) {
            return NGX_ERROR;
        }
    }

    sc->variables = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_script_done(ngx_http_script_compile_t *sc)
{
    ngx_str_t    zero;
    uintptr_t   *code;

    if (sc->zero) {

        zero.len = 1;
        zero.data = (u_char *) "\0";

        if (ngx_http_script_add_copy_code(sc, &zero, 0) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (sc->conf_prefix || sc->root_prefix) {
        if (ngx_http_script_add_full_name_code(sc) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (sc->complete_lengths) {
        code = ngx_http_script_add_code(*sc->lengths, sizeof(uintptr_t), NULL);
        if (code == NULL) {
            return NGX_ERROR;
        }

        *code = (uintptr_t) NULL;
    }

    if (sc->complete_values) {
        code = ngx_http_script_add_code(*sc->values, sizeof(uintptr_t),
                                        &sc->main);
        if (code == NULL) {
            return NGX_ERROR;
        }

        *code = (uintptr_t) NULL;
    }

    return NGX_OK;
}


void *
ngx_http_script_start_code(ngx_pool_t *pool, ngx_array_t **codes, size_t size)
{
    if (*codes == NULL) {
        *codes = ngx_array_create(pool, 256, 1);
        if (*codes == NULL) {
            return NULL;
        }
    }

    return ngx_array_push_n(*codes, size);
}


/*
 * 从codes数组中分配size个字节的内存空间
 *
 * code代表数组扩容前从数组中分配出去的一个内存地址,如果这次分配内存发生了扩容,则code就代表其本身扩容后的地址
 */
void *
ngx_http_script_add_code(ngx_array_t *codes, size_t size, void *code)
{
    u_char  *elts, **p;
    void    *new;

    elts = codes->elts;

    new = ngx_array_push_n(codes, size);
    if (new == NULL) {
        return NULL;
    }

    if (code) {
        if (elts != codes->elts) {
        	/* 不相等代表数组扩容了 */

        	/*
        	 *   code		 p
        	 *   -----	   -----
        	 *   | * |	   | * |
        	 *   -----     -----
        	 *   	 \     /
        	 *   	  -----
        	 *   	  | * |
        	 *   	  -----
        	 *   		\
        	 *   		-------------
        	 *   		| 	结构体	|
        	 *   		-------------
        	 *
        	 *  因为发生扩容了,所以从codes中分配出去的内存地址也会发生改变,如果在扩容之后不会对原来分配出去的地址有依赖,
        	 *  则没有任何问题,但是如果有依赖则会发生错误,因为codes内中所有内存地址因为扩容的关系都发生了改变,下面的代码
        	 *  就是为了解决扩容后仍然可以得到正确地址的方法。
        	 *
        	 *  举个例子:
        	 *	假设codes初始内存结构如下图:
        	 *		codes:
        	 *			--------- 0x0
        	 *			| 1byte	|
        	 *			--------- 0x1
        	 *			| 1byte	|
        	 *			--------- 0x2
        	 *			| 1byte |
        	 *			---------
        	 *	假设code需要占用两个字节空间,则需要用到codes中的0x1和0x2两个地址,而code的地址则为0x1。
        	 *	假设这是需要为code2也分配连个字节空间,这时候codes就需要扩容了,假设扩容后codes的内存结构图如下:
        	 *		codes:
        	 *			--------- 0x5
        	 *			| 1byte	|
        	 *			--------- 0x6
        	 *			| 1byte	|
        	 *			--------- 0x7
        	 *			| 1byte |
        	 *			--------- 0x8
        	 *			| 1byte |
        	 *			--------- 0x9
        	 *			| 1byte |
        	 *			---------
        	 *	从上图可以看到这个时候code的内存其实地址就发生了改变,但是它在数组中的位置是不会改变的,所以我们可以
        	 *	通过几种方式计算出code在数组扩容后的地址。
        	 *
        	 *  第一种是下面ngx代码使用的方式,用扩容后的数组首地址(codes->elts)减去未扩容前数组的首地址(elts),
        	 *  这样就可以计算出整个数组的偏移量,然后在加上code原来的地址就可以算出它现在的地址。
        	 *
        	 *  另一种我们可以先计算出code在codes中的偏移量,即code的内存地址减去codes未扩容前的首地址(elts),然后
        	 *  再用扩容后的首地址(codes->elts)加上code原来的地址,这样就可以计算出它现在的地址。
        	 *  code(扩容前地址) - elts(codes扩容前首地址) + codes->elts(codes扩容后首地址);
        	 *
        	 *  p = code;
        	 *  *p = *p - elts + (u_char *)codes->elts;
        	 *
        	 */
            p = code;
            *p += (u_char *) codes->elts - elts;
            // *p = *p +   (u_char *) codes->elts - elts;
        }
    }

    return new;
}


/*
 * 这是一个用来添加计算纯文本值的脚本方法,也就是说该方法生成了一个脚本,这个脚本用来动态计算纯文本值的,比如
 *    return  200 "I am $uri";
 * 其中"I am $uri"在ngx中是一个复杂值,这个值的计算会被编译成脚本,对于纯文本值"I am "会使用
 * ngx_http_script_add_copy_code()方法来生成计算脚本,而对于"$uri"这个变量则需要使用
 * ngx_http_script_add_var_code()方法来生成计算脚本,最后通过脚本引擎执行脚本的形式把整个复杂值
 * 给事实的计算出来
 */
static ngx_int_t
ngx_http_script_add_copy_code(ngx_http_script_compile_t *sc, ngx_str_t *value,
    ngx_uint_t last)
{
    u_char                       *p;
    size_t                        size, len, zero;
    ngx_http_script_copy_code_t  *code;

    zero = (sc->zero && last);
    len = value->len + zero;

    /*
     * 向*sc->lengths数组中添加一个ngx_http_script_copy_code_t脚本对象
     *
     * 这个脚本对象只是为了计算长度吗? TODO 计算的是谁的长度?
     */
    code = ngx_http_script_add_code(*sc->lengths,
                                    sizeof(ngx_http_script_copy_code_t), NULL);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = (ngx_http_script_code_pt) ngx_http_script_copy_len_code;
    code->len = len;

    size = (sizeof(ngx_http_script_copy_code_t) + len + sizeof(uintptr_t) - 1)
            & ~(sizeof(uintptr_t) - 1);

    /*
     * 向*sc->values数组中添加一个ngx_http_script_copy_code_t脚本对象
     *
     * 如果在添加的过程中*sc->values发生了扩容,那么*sc->main中的指针就是扩容后的新指针
     *
     * 从size的计算方式可以看出*sc->values中存放了真正的文本信息,比如
     *    return 200 "I am $uri";
     * 此时*sc->values应该是如下结构:
     *   ---------------------------------------------------------
     *   | sizeof(ngx_http_script_copy_code_t) |  len   |
     *   ---------------------------------------------------------
     * 而ngx_http_script_copy_code()方法知道如何去处理这个结构
     */
    code = ngx_http_script_add_code(*sc->values, size, &sc->main);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = ngx_http_script_copy_code;
    code->len = len;

    /*
     * 这一步是为了把"I am "字符 *sc->values中,拷贝完成后*sc->values结构如下:
     *   ---------------------------------------------------------
     *   | sizeof(ngx_http_script_copy_code_t) | "I am " |
     *   ---------------------------------------------------------
     */
    p = ngx_cpymem((u_char *) code + sizeof(ngx_http_script_copy_code_t),
                   value->data, value->len);

    if (zero) {
        *p = '\0';
        sc->zero = 0;
    }

    return NGX_OK;
}


/*
 * 返回ngx_http_script_copy_code_t脚本对象中code->len的值
 * 并且把e->ip移动sizeof(ngx_http_script_copy_code_t)个字节
 */
size_t
ngx_http_script_copy_len_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_copy_code_t  *code;

    code = (ngx_http_script_copy_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_copy_code_t);

    return code->len;
}


void
ngx_http_script_copy_code(ngx_http_script_engine_t *e)
{
    u_char                       *p;
    ngx_http_script_copy_code_t  *code;

    code = (ngx_http_script_copy_code_t *) e->ip;

    p = e->pos;

    if (!e->skip) {
        e->pos = ngx_copy(p, e->ip + sizeof(ngx_http_script_copy_code_t),
                          code->len);
    }

    e->ip += sizeof(ngx_http_script_copy_code_t)
          + ((code->len + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script copy: \"%*s\"", e->pos - p, p);
}


/*
 * 这是一个用来添加计算变量值的脚本方法,也就是说该方法生成了一个脚本,这个脚本用来动态计算变量值的,比如
 *    return  200 "I am $uri";
 * 其中"I am $uri"在ngx中是一个复杂值,这个值的计算会被编译成脚本,对于纯文本值"I am "会使用
 * ngx_http_script_add_copy_code()方法来生成计算脚本,而对于"$uri"这个变量则需要使用
 * ngx_http_script_add_var_code()方法来生成计算脚本,最后通过脚本引擎执行脚本的形式把整个复杂值
 * 给事实的计算出来
 */
static ngx_int_t
ngx_http_script_add_var_code(ngx_http_script_compile_t *sc, ngx_str_t *name)
{
    ngx_int_t                    index, *p;
    ngx_http_script_var_code_t  *code;

    /*
     * 取出这个变量的索引(下标)
     */
    index = ngx_http_get_variable_index(sc->cf, name);

    if (index == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (sc->flushes) {
        p = ngx_array_push(*sc->flushes);
        if (p == NULL) {
            return NGX_ERROR;
        }

        /*
         * 变量在cmcf->variables数组中的下标,放到*sc->flushes数组中
         *
         * 假设解析的是如下复杂值,并且变量"$uri"在cmcf->variables数组中的下标是8
         *   return 200 "I am $uri $uri";
         * 则此时sc->flushes结构如下:
         *   sc->flushes
         *     -----
         *     | * |
         *     -----
         *       \ p
         *       -----
         *       | * |
         *       -----
         *         \
         *          ---------
         *          | 8 | 8 |
         *          ---------
         */
        *p = index;
    }

    code = ngx_http_script_add_code(*sc->lengths,
                                    sizeof(ngx_http_script_var_code_t), NULL);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = (ngx_http_script_code_pt) ngx_http_script_copy_var_len_code;
    code->index = (uintptr_t) index;

    code = ngx_http_script_add_code(*sc->values,
                                    sizeof(ngx_http_script_var_code_t),
                                    &sc->main);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = ngx_http_script_copy_var_code;
    code->index = (uintptr_t) index;

    return NGX_OK;
}


size_t
ngx_http_script_copy_var_len_code(ngx_http_script_engine_t *e)
{
    ngx_http_variable_value_t   *value;
    ngx_http_script_var_code_t  *code;

    code = (ngx_http_script_var_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_var_code_t);

    if (e->flushed) {
        value = ngx_http_get_indexed_variable(e->request, code->index);

    } else {
        value = ngx_http_get_flushed_variable(e->request, code->index);
    }

    if (value && !value->not_found) {
        return value->len;
    }

    return 0;
}


void
ngx_http_script_copy_var_code(ngx_http_script_engine_t *e)
{
    u_char                      *p;
    ngx_http_variable_value_t   *value;
    ngx_http_script_var_code_t  *code;

    code = (ngx_http_script_var_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_var_code_t);

    if (!e->skip) {

        if (e->flushed) {
            value = ngx_http_get_indexed_variable(e->request, code->index);

        } else {
            value = ngx_http_get_flushed_variable(e->request, code->index);
        }

        if (value && !value->not_found) {
            p = e->pos;
            e->pos = ngx_copy(p, value->data, value->len);

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP,
                           e->request->connection->log, 0,
                           "http script var: \"%*s\"", e->pos - p, p);
        }
    }
}


static ngx_int_t
ngx_http_script_add_args_code(ngx_http_script_compile_t *sc)
{
    uintptr_t   *code;

    code = ngx_http_script_add_code(*sc->lengths, sizeof(uintptr_t), NULL);
    if (code == NULL) {
        return NGX_ERROR;
    }

    *code = (uintptr_t) ngx_http_script_mark_args_code;

    code = ngx_http_script_add_code(*sc->values, sizeof(uintptr_t), &sc->main);
    if (code == NULL) {
        return NGX_ERROR;
    }

    *code = (uintptr_t) ngx_http_script_start_args_code;

    return NGX_OK;
}


size_t
ngx_http_script_mark_args_code(ngx_http_script_engine_t *e)
{
    e->is_args = 1;
    e->ip += sizeof(uintptr_t);

    return 1;
}


void
ngx_http_script_start_args_code(ngx_http_script_engine_t *e)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script args");

    e->is_args = 1;
    e->args = e->pos;
    e->ip += sizeof(uintptr_t);
}


#if (NGX_PCRE)

void
ngx_http_script_regex_start_code(ngx_http_script_engine_t *e)
{
    size_t                         len;
    ngx_int_t                      rc;
    ngx_uint_t                     n;
    ngx_http_request_t            *r;
    ngx_http_script_engine_t       le;
    ngx_http_script_len_code_pt    lcode;
    ngx_http_script_regex_code_t  *code;

    code = (ngx_http_script_regex_code_t *) e->ip;

    r = e->request;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http script regex: \"%V\"", &code->name);

    if (code->uri) {
        e->line = r->uri;
    } else {
        e->sp--;
        e->line.len = e->sp->len;
        e->line.data = e->sp->data;
    }

    rc = ngx_http_regex_exec(r, code->regex, &e->line);

    if (rc == NGX_DECLINED) {
        if (e->log || (r->connection->log->log_level & NGX_LOG_DEBUG_HTTP)) {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                          "\"%V\" does not match \"%V\"",
                          &code->name, &e->line);
        }

        r->ncaptures = 0;

        if (code->test) {
            if (code->negative_test) {
                e->sp->len = 1;
                e->sp->data = (u_char *) "1";

            } else {
                e->sp->len = 0;
                e->sp->data = (u_char *) "";
            }

            e->sp++;

            e->ip += sizeof(ngx_http_script_regex_code_t);
            return;
        }

        e->ip += code->next;
        return;
    }

    if (rc == NGX_ERROR) {
        e->ip = ngx_http_script_exit;
        e->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return;
    }

    if (e->log || (r->connection->log->log_level & NGX_LOG_DEBUG_HTTP)) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "\"%V\" matches \"%V\"", &code->name, &e->line);
    }

    if (code->test) {
        if (code->negative_test) {
            e->sp->len = 0;
            e->sp->data = (u_char *) "";

        } else {
            e->sp->len = 1;
            e->sp->data = (u_char *) "1";
        }

        e->sp++;

        e->ip += sizeof(ngx_http_script_regex_code_t);
        return;
    }

    if (code->status) {
        e->status = code->status;

        if (!code->redirect) {
            e->ip = ngx_http_script_exit;
            return;
        }
    }

    if (code->uri) {
        r->internal = 1;
        r->valid_unparsed_uri = 0;

        if (code->break_cycle) {
            r->valid_location = 0;
            r->uri_changed = 0;

        } else {
            r->uri_changed = 1;
        }
    }

    if (code->lengths == NULL) {
        e->buf.len = code->size;

        if (code->uri) {
            if (r->ncaptures && (r->quoted_uri || r->plus_in_uri)) {
                e->buf.len += 2 * ngx_escape_uri(NULL, r->uri.data, r->uri.len,
                                                 NGX_ESCAPE_ARGS);
            }
        }

        for (n = 2; n < r->ncaptures; n += 2) {
            e->buf.len += r->captures[n + 1] - r->captures[n];
        }

    } else {
        ngx_memzero(&le, sizeof(ngx_http_script_engine_t));

        le.ip = code->lengths->elts;
        le.line = e->line;
        le.request = r;
        le.quote = code->redirect;

        len = 0;

        while (*(uintptr_t *) le.ip) {
            lcode = *(ngx_http_script_len_code_pt *) le.ip;
            len += lcode(&le);
        }

        e->buf.len = len;
    }

    if (code->add_args && r->args.len) {
        e->buf.len += r->args.len + 1;
    }

    e->buf.data = ngx_pnalloc(r->pool, e->buf.len);
    if (e->buf.data == NULL) {
        e->ip = ngx_http_script_exit;
        e->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return;
    }

    e->quote = code->redirect;

    e->pos = e->buf.data;

    e->ip += sizeof(ngx_http_script_regex_code_t);
}


void
ngx_http_script_regex_end_code(ngx_http_script_engine_t *e)
{
    u_char                            *dst, *src;
    ngx_http_request_t                *r;
    ngx_http_script_regex_end_code_t  *code;

    code = (ngx_http_script_regex_end_code_t *) e->ip;

    r = e->request;

    e->quote = 0;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http script regex end");

    if (code->redirect) {

        dst = e->buf.data;
        src = e->buf.data;

        ngx_unescape_uri(&dst, &src, e->pos - e->buf.data,
                         NGX_UNESCAPE_REDIRECT);

        if (src < e->pos) {
            dst = ngx_movemem(dst, src, e->pos - src);
        }

        e->pos = dst;

        if (code->add_args && r->args.len) {
            *e->pos++ = (u_char) (code->args ? '&' : '?');
            e->pos = ngx_copy(e->pos, r->args.data, r->args.len);
        }

        e->buf.len = e->pos - e->buf.data;

        if (e->log || (r->connection->log->log_level & NGX_LOG_DEBUG_HTTP)) {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                          "rewritten redirect: \"%V\"", &e->buf);
        }

        ngx_http_clear_location(r);

        r->headers_out.location = ngx_list_push(&r->headers_out.headers);
        if (r->headers_out.location == NULL) {
            e->ip = ngx_http_script_exit;
            e->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
            return;
        }

        r->headers_out.location->hash = 1;
        ngx_str_set(&r->headers_out.location->key, "Location");
        r->headers_out.location->value = e->buf;

        e->ip += sizeof(ngx_http_script_regex_end_code_t);
        return;
    }

    if (e->args) {
        e->buf.len = e->args - e->buf.data;

        if (code->add_args && r->args.len) {
            *e->pos++ = '&';
            e->pos = ngx_copy(e->pos, r->args.data, r->args.len);
        }

        r->args.len = e->pos - e->args;
        r->args.data = e->args;

        e->args = NULL;

    } else {
        e->buf.len = e->pos - e->buf.data;

        if (!code->add_args) {
            r->args.len = 0;
        }
    }

    if (e->log || (r->connection->log->log_level & NGX_LOG_DEBUG_HTTP)) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "rewritten data: \"%V\", args: \"%V\"",
                      &e->buf, &r->args);
    }

    if (code->uri) {
    	// 改写uri
        r->uri = e->buf;

        if (r->uri.len == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the rewritten URI has a zero length");
            e->ip = ngx_http_script_exit;
            e->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
            return;
        }

        ngx_http_set_exten(r);
    }

    e->ip += sizeof(ngx_http_script_regex_end_code_t);
}


static ngx_int_t
ngx_http_script_add_capture_code(ngx_http_script_compile_t *sc, ngx_uint_t n)
{
    ngx_http_script_copy_capture_code_t  *code;

    code = ngx_http_script_add_code(*sc->lengths,
                                    sizeof(ngx_http_script_copy_capture_code_t),
                                    NULL);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = (ngx_http_script_code_pt)
                      ngx_http_script_copy_capture_len_code;
    code->n = 2 * n;


    code = ngx_http_script_add_code(*sc->values,
                                    sizeof(ngx_http_script_copy_capture_code_t),
                                    &sc->main);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = ngx_http_script_copy_capture_code;
    code->n = 2 * n;

    if (sc->ncaptures < n) {
        sc->ncaptures = n;
    }

    return NGX_OK;
}


size_t
ngx_http_script_copy_capture_len_code(ngx_http_script_engine_t *e)
{
    int                                  *cap;
    u_char                               *p;
    ngx_uint_t                            n;
    ngx_http_request_t                   *r;
    ngx_http_script_copy_capture_code_t  *code;

    r = e->request;

    code = (ngx_http_script_copy_capture_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_copy_capture_code_t);

    n = code->n;

    if (n < r->ncaptures) {

        cap = r->captures;

        if ((e->is_args || e->quote)
            && (e->request->quoted_uri || e->request->plus_in_uri))
        {
            p = r->captures_data;

            return cap[n + 1] - cap[n]
                   + 2 * ngx_escape_uri(NULL, &p[cap[n]], cap[n + 1] - cap[n],
                                        NGX_ESCAPE_ARGS);
        } else {
            return cap[n + 1] - cap[n];
        }
    }

    return 0;
}


void
ngx_http_script_copy_capture_code(ngx_http_script_engine_t *e)
{
    int                                  *cap;
    u_char                               *p, *pos;
    ngx_uint_t                            n;
    ngx_http_request_t                   *r;
    ngx_http_script_copy_capture_code_t  *code;

    r = e->request;

    code = (ngx_http_script_copy_capture_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_copy_capture_code_t);

    n = code->n;

    pos = e->pos;

    if (n < r->ncaptures) {

        cap = r->captures;
        p = r->captures_data;

        if ((e->is_args || e->quote)
            && (e->request->quoted_uri || e->request->plus_in_uri))
        {
            e->pos = (u_char *) ngx_escape_uri(pos, &p[cap[n]],
                                               cap[n + 1] - cap[n],
                                               NGX_ESCAPE_ARGS);
        } else {
            e->pos = ngx_copy(pos, &p[cap[n]], cap[n + 1] - cap[n]);
        }
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script capture: \"%*s\"", e->pos - pos, pos);
}

#endif


static ngx_int_t
ngx_http_script_add_full_name_code(ngx_http_script_compile_t *sc)
{
    ngx_http_script_full_name_code_t  *code;

    code = ngx_http_script_add_code(*sc->lengths,
                                    sizeof(ngx_http_script_full_name_code_t),
                                    NULL);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = (ngx_http_script_code_pt) ngx_http_script_full_name_len_code;
    code->conf_prefix = sc->conf_prefix;

    code = ngx_http_script_add_code(*sc->values,
                                    sizeof(ngx_http_script_full_name_code_t),
                                    &sc->main);
    if (code == NULL) {
        return NGX_ERROR;
    }

    code->code = ngx_http_script_full_name_code;
    code->conf_prefix = sc->conf_prefix;

    return NGX_OK;
}


static size_t
ngx_http_script_full_name_len_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_full_name_code_t  *code;

    code = (ngx_http_script_full_name_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_full_name_code_t);

    return code->conf_prefix ? ngx_cycle->conf_prefix.len:
                               ngx_cycle->prefix.len;
}


static void
ngx_http_script_full_name_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_full_name_code_t  *code;

    ngx_str_t  value, *prefix;

    code = (ngx_http_script_full_name_code_t *) e->ip;

    value.data = e->buf.data;
    value.len = e->pos - e->buf.data;

    prefix = code->conf_prefix ? (ngx_str_t *) &ngx_cycle->conf_prefix:
                                 (ngx_str_t *) &ngx_cycle->prefix;

    if (ngx_get_full_name(e->request->pool, prefix, &value) != NGX_OK) {
        e->ip = ngx_http_script_exit;
        e->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return;
    }

    e->buf = value;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script fullname: \"%V\"", &value);

    e->ip += sizeof(ngx_http_script_full_name_code_t);
}


void
ngx_http_script_return_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_return_code_t  *code;

    code = (ngx_http_script_return_code_t *) e->ip;

    if (code->status < NGX_HTTP_BAD_REQUEST
        || code->text.value.len
        || code->text.lengths)
    {
    	// 提前结束,不再走后面的阶段了
        e->status = ngx_http_send_response(e->request, code->status, NULL,
                                           &code->text);
    } else {
        e->status = code->status;
    }

    e->ip = ngx_http_script_exit;
}


void
ngx_http_script_break_code(ngx_http_script_engine_t *e)
{
    e->request->uri_changed = 0;

    e->ip = ngx_http_script_exit;
}


void
ngx_http_script_if_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_if_code_t  *code;

    code = (ngx_http_script_if_code_t *) e->ip;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script if");

    e->sp--;

    if (e->sp->len && (e->sp->len != 1 || e->sp->data[0] != '0')) {
        if (code->loc_conf) {
            e->request->loc_conf = code->loc_conf;
            ngx_http_update_location_config(e->request);
        }

        e->ip += sizeof(ngx_http_script_if_code_t);
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script if: false");

    e->ip += code->next;
}


void
ngx_http_script_equal_code(ngx_http_script_engine_t *e)
{
    ngx_http_variable_value_t  *val, *res;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script equal");

    e->sp--;
    val = e->sp;
    res = e->sp - 1;

    e->ip += sizeof(uintptr_t);

    if (val->len == res->len
        && ngx_strncmp(val->data, res->data, res->len) == 0)
    {
        *res = ngx_http_variable_true_value;
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script equal: no");

    *res = ngx_http_variable_null_value;
}


void
ngx_http_script_not_equal_code(ngx_http_script_engine_t *e)
{
    ngx_http_variable_value_t  *val, *res;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script not equal");

    e->sp--;
    val = e->sp;
    res = e->sp - 1;

    e->ip += sizeof(uintptr_t);

    if (val->len == res->len
        && ngx_strncmp(val->data, res->data, res->len) == 0)
    {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                       "http script not equal: no");

        *res = ngx_http_variable_null_value;
        return;
    }

    *res = ngx_http_variable_true_value;
}


void
ngx_http_script_file_code(ngx_http_script_engine_t *e)
{
    ngx_str_t                     path;
    ngx_http_request_t           *r;
    ngx_open_file_info_t          of;
    ngx_http_core_loc_conf_t     *clcf;
    ngx_http_variable_value_t    *value;
    ngx_http_script_file_code_t  *code;

    value = e->sp - 1;

    code = (ngx_http_script_file_code_t *) e->ip;
    e->ip += sizeof(ngx_http_script_file_code_t);

    path.len = value->len - 1;
    path.data = value->data;

    r = e->request;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http script file op %p \"%V\"", code->op, &path);

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    ngx_memzero(&of, sizeof(ngx_open_file_info_t));

    of.read_ahead = clcf->read_ahead;
    of.directio = clcf->directio;
    of.valid = clcf->open_file_cache_valid;
    of.min_uses = clcf->open_file_cache_min_uses;
    of.test_only = 1;
    of.errors = clcf->open_file_cache_errors;
    of.events = clcf->open_file_cache_events;

    if (ngx_http_set_disable_symlinks(r, clcf, &path, &of) != NGX_OK) {
        e->ip = ngx_http_script_exit;
        e->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return;
    }

    if (ngx_open_cached_file(clcf->open_file_cache, &path, &of, r->pool)
        != NGX_OK)
    {
        if (of.err != NGX_ENOENT
            && of.err != NGX_ENOTDIR
            && of.err != NGX_ENAMETOOLONG)
        {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, of.err,
                          "%s \"%s\" failed", of.failed, value->data);
        }

        switch (code->op) {

        case ngx_http_script_file_plain:
        case ngx_http_script_file_dir:
        case ngx_http_script_file_exists:
        case ngx_http_script_file_exec:
             goto false_value;

        case ngx_http_script_file_not_plain:
        case ngx_http_script_file_not_dir:
        case ngx_http_script_file_not_exists:
        case ngx_http_script_file_not_exec:
             goto true_value;
        }

        goto false_value;
    }

    switch (code->op) {
    case ngx_http_script_file_plain:
        if (of.is_file) {
             goto true_value;
        }
        goto false_value;

    case ngx_http_script_file_not_plain:
        if (of.is_file) {
            goto false_value;
        }
        goto true_value;

    case ngx_http_script_file_dir:
        if (of.is_dir) {
             goto true_value;
        }
        goto false_value;

    case ngx_http_script_file_not_dir:
        if (of.is_dir) {
            goto false_value;
        }
        goto true_value;

    case ngx_http_script_file_exists:
        if (of.is_file || of.is_dir || of.is_link) {
             goto true_value;
        }
        goto false_value;

    case ngx_http_script_file_not_exists:
        if (of.is_file || of.is_dir || of.is_link) {
            goto false_value;
        }
        goto true_value;

    case ngx_http_script_file_exec:
        if (of.is_exec) {
             goto true_value;
        }
        goto false_value;

    case ngx_http_script_file_not_exec:
        if (of.is_exec) {
            goto false_value;
        }
        goto true_value;
    }

false_value:

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http script file op false");

    *value = ngx_http_variable_null_value;
    return;

true_value:

    *value = ngx_http_variable_true_value;
    return;
}


void
ngx_http_script_complex_value_code(ngx_http_script_engine_t *e)
{
    size_t                                 len;
    ngx_http_script_engine_t               le;
    ngx_http_script_len_code_pt            lcode;
    ngx_http_script_complex_value_code_t  *code;

    code = (ngx_http_script_complex_value_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_complex_value_code_t);

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script complex value");

    ngx_memzero(&le, sizeof(ngx_http_script_engine_t));

    le.ip = code->lengths->elts;
    le.line = e->line;
    le.request = e->request;
    le.quote = e->quote;

    for (len = 0; *(uintptr_t *) le.ip; len += lcode(&le)) {
        lcode = *(ngx_http_script_len_code_pt *) le.ip;
    }

    e->buf.len = len;
    e->buf.data = ngx_pnalloc(e->request->pool, len);
    if (e->buf.data == NULL) {
        e->ip = ngx_http_script_exit;
        e->status = NGX_HTTP_INTERNAL_SERVER_ERROR;
        return;
    }

    e->pos = e->buf.data;

    e->sp->len = e->buf.len;
    e->sp->data = e->buf.data;
    e->sp++;
}


/*
 * 该方法可以理解为引擎的指令,会基于栈(e->sp)来操作
 *
 * 取出变量值将其赋值给e->sp->data,比如有如下指令:
 * 		set $a $bb$cc
 * 在该方法的作用就是将变量值($bb$cc)赋值给e->sp->data
 */
void
ngx_http_script_value_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_value_code_t  *code;

    // 该结构体存放了变量的值
    code = (ngx_http_script_value_code_t *) e->ip;

    // 做相应的偏移,脚本引擎可以继续执行后续的代码
    e->ip += sizeof(ngx_http_script_value_code_t);

    e->sp->len = code->text_len;
    e->sp->data = (u_char *) code->text_data;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script value: \"%v\"", e->sp);

    // 压栈(栈顶数据为空)
    e->sp++;
}


/*
 * 该方法可以理解为引擎的指令,会基于栈(e->sp)来操作
 *
 * 为变量赋值,如有如下指令:
 * 		set $a $bb$cc
 * 则该方法的作用是为变量$a赋值(e->sp->data),其中e->sp->data中存放的就是$bb$cc解析后的值
 */
void
ngx_http_script_set_var_code(ngx_http_script_engine_t *e)
{
    ngx_http_request_t          *r;
    ngx_http_script_var_code_t  *code;

    // 该结构体存放了变量在variables中的下标
    code = (ngx_http_script_var_code_t *) e->ip;

    // 做相应的偏移,脚本引擎可以继续执行后续的代码
    e->ip += sizeof(ngx_http_script_var_code_t);

    r = e->request;

    /*
     * 在ngx_http_script_value_code()方法中做了e->sp++,相当于把栈加一,
     * 这里是要取出栈中的值,所以这里对栈减一
     *
     * 弹栈(取出栈顶数据)
     */
    e->sp--;

    r->variables[code->index].len = e->sp->len;
    r->variables[code->index].valid = 1;
    r->variables[code->index].no_cacheable = 0;
    r->variables[code->index].not_found = 0;
    // 设置变量值
    r->variables[code->index].data = e->sp->data;

#if (NGX_DEBUG)
    {
    ngx_http_variable_t        *v;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);

    v = cmcf->variables.elts;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script set $%V", &v[code->index].name);
    }
#endif
}


void
ngx_http_script_var_set_handler_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_var_handler_code_t  *code;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script set var handler");

    code = (ngx_http_script_var_handler_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_var_handler_code_t);

    e->sp--;

    code->handler(e->request, e->sp, code->data);
}


void
ngx_http_script_var_code(ngx_http_script_engine_t *e)
{
    ngx_http_variable_value_t   *value;
    ngx_http_script_var_code_t  *code;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script var");

    code = (ngx_http_script_var_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_var_code_t);

    value = ngx_http_get_flushed_variable(e->request, code->index);

    if (value && !value->not_found) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                       "http script var: \"%v\"", value);

        *e->sp = *value;
        e->sp++;

        return;
    }

    *e->sp = ngx_http_variable_null_value;
    e->sp++;
}


void
ngx_http_script_nop_code(ngx_http_script_engine_t *e)
{
    e->ip += sizeof(uintptr_t);
}
