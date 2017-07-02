
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>

#define NGX_CONF_BUFFER  4096

static ngx_int_t ngx_conf_handler(ngx_conf_t *cf, ngx_int_t last);
static ngx_int_t ngx_conf_read_token(ngx_conf_t *cf);
static void ngx_conf_flush_files(ngx_cycle_t *cycle);


static ngx_command_t  ngx_conf_commands[] = {

    { ngx_string("include"),
      NGX_ANY_CONF|NGX_CONF_TAKE1,
      ngx_conf_include,
      0,
      0,
      NULL },

      ngx_null_command
};


ngx_module_t  ngx_conf_module = {
    NGX_MODULE_V1,
    NULL,                                  /* module context */
    ngx_conf_commands,                     /* module directives */
    NGX_CONF_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_conf_flush_files,                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/* The eight fixed arguments */
/* 指令可以带的八种参数个数 */
static ngx_uint_t argument_number[] = {
    NGX_CONF_NOARGS,
    NGX_CONF_TAKE1,
    NGX_CONF_TAKE2,
    NGX_CONF_TAKE3,
    NGX_CONF_TAKE4,
    NGX_CONF_TAKE5,
    NGX_CONF_TAKE6,
    NGX_CONF_TAKE7
};


/*
 * 处理 -g 参数传入的命令
 */
char *
ngx_conf_param(ngx_conf_t *cf)
{
    char             *rv;
    ngx_str_t        *param;
    ngx_buf_t         b;
    ngx_conf_file_t   conf_file;

    param = &cf->cycle->conf_param;

    if (param->len == 0) {
        return NGX_CONF_OK;
    }

    ngx_memzero(&conf_file, sizeof(ngx_conf_file_t));

    ngx_memzero(&b, sizeof(ngx_buf_t));

    b.start = param->data;
    b.pos = param->data;
    b.last = param->data + param->len;
    b.end = b.last;
    b.temporary = 1;

    // ngx_conf_parse方法中用NGX_INVALID_FILE标记判断是否是解析文件
    conf_file.file.fd = NGX_INVALID_FILE;
    conf_file.file.name.data = NULL;
    conf_file.line = 0;

    cf->conf_file = &conf_file;
    cf->conf_file->buffer = &b;

    rv = ngx_conf_parse(cf, NULL);

    cf->conf_file = NULL;

    return rv;
}


/**
 * 解析配置指令,并执行配置指令对应的方法
 *
 * 该方法解析三种类型的配置:
 * 1.解析配置文件中的内容
 * 2.解析带花括号中的指令
 * 3.解析用-g参数输入的命令,-g入参只能输入简单的指令格式,不能输入比如带花括号的复杂指令
 *
 * 对于 "xxx {}" 这样的指令形式,如果花括号内的内容不是指令,则需要在调用该方法之前设置
 * cf->handler方法,有这个handler来处理。
 */
char *
ngx_conf_parse(ngx_conf_t *cf, ngx_str_t *filename)
{
    char             *rv;
    u_char           *p;
    off_t             size;
    ngx_fd_t          fd;
    ngx_int_t         rc;
    ngx_buf_t         buf, *tbuf;
    ngx_conf_file_t  *prev, conf_file;
    ngx_conf_dump_t  *cd;
    enum {
    	// 默认解析配置文件
        parse_file = 0,
		// 解析块配置,带花括号的
        parse_block,
		// 解析-g参数传入的命令
        parse_param
    } type;

#if (NGX_SUPPRESS_WARN)
    fd = NGX_INVALID_FILE;
    prev = NULL;
#endif

    // 解析配置文件
    if (filename) {

        /* open configuration file */

    	// 打开配置文件,比如nginx.conf,也有可能是include指令指定的文件
        fd = ngx_open_file(filename->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
        if (fd == NGX_INVALID_FILE) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                               ngx_open_file_n " \"%s\" failed",
                               filename->data);
            return NGX_CONF_ERROR;
        }

        /*
         * 保留前一个配置文件信息(比如前一个文件的解析位置等)
         * 如果nginx.conf中包含include指令,则这里的前一个配置文件就有可能是nginx.conf了
         *
         * 解析完毕后要把prev还回到cf->conf_file中
         *
         * 在用一个文件中,cf->conf_file->buffer是公共的,因为他是一个指针,解析过程中不会改变
         *
         */
        prev = cf->conf_file;

        // 把在当前栈内分配内存的ngx_conf_file_t对象赋值给cf->conf_file
        cf->conf_file = &conf_file;

        // 调用系统函数fstat,用来获取打开的配置文件(fd)的一些基本信息(比如i-node节点号)
        if (ngx_fd_info(fd, &cf->conf_file->file.info) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, ngx_errno,
                          ngx_fd_info_n " \"%s\" failed", filename->data);
        }

        /*
         * 同样,将在当前站内分配内存的ngx_buf_t对象赋值给cf->conf_file->buffer
         * 这里可以认为一个文件对应一个buffer对象
         */
        cf->conf_file->buffer = &buf;

        // 为buffer分配缓存空间,这里分配了NGX_CONF_BUFFER个字节
        buf.start = ngx_alloc(NGX_CONF_BUFFER, cf->log);
        if (buf.start == NULL) {
            goto failed;
        }

        // 设置缓存个各个指针
        buf.pos = buf.start;
        buf.last = buf.start;
        buf.end = buf.last + NGX_CONF_BUFFER;
        // 为1表示该缓存内容可以被修改
        buf.temporary = 1;

        // 要解析的文件的描述符
        cf->conf_file->file.fd = fd;
        // 要解析的文件名字长度
        cf->conf_file->file.name.len = filename->len;
        // 要解析的文件名字
        cf->conf_file->file.name.data = filename->data;
        // 要解析文件位置偏移量,因为这里是刚打开该文件,所以设置为0
        cf->conf_file->file.offset = 0;
        cf->conf_file->file.log = cf->log;
        // 当前文件解析到第几行
        cf->conf_file->line = 1;

        // 解析类型设置为解析配置文件
        type = parse_file;

        if (ngx_dump_config  // 入参以-T开头,测试和dump
#if (NGX_DEBUG)
            || 1
#endif
           )
        {
            p = ngx_pstrdup(cf->cycle->pool, filename);
            if (p == NULL) {
                goto failed;
            }

            size = ngx_file_size(&cf->conf_file->file.info);

            tbuf = ngx_create_temp_buf(cf->cycle->pool, (size_t) size);
            if (tbuf == NULL) {
                goto failed;
            }

            cd = ngx_array_push(&cf->cycle->config_dump);
            if (cd == NULL) {
                goto failed;
            }

            cd->name.len = filename->len;
            cd->name.data = p;
            cd->buffer = tbuf;

            cf->conf_file->dump = tbuf;

        } else {
            cf->conf_file->dump = NULL;
        }

    } else if (cf->conf_file->file.fd != NGX_INVALID_FILE) {

    	// 解析花括号中的命令
        type = parse_block;

    } else {
    	// 解析以-g入参输入的命令
        type = parse_param;
    }


    // 开始解析命令
    for ( ;; ) {
        rc = ngx_conf_read_token(cf);

        /*
         * ngx_conf_read_token() may return
         *
         *    NGX_ERROR             there is error
         *    NGX_OK                the token terminated by ";" was found
         *    NGX_CONF_BLOCK_START  the token terminated by "{" was found
         *    NGX_CONF_BLOCK_DONE   the "}" was found
         *    NGX_CONF_FILE_DONE    the configuration file is done
         */

        if (rc == NGX_ERROR) {
            goto done;
        }

        if (rc == NGX_CONF_BLOCK_DONE) {

            if (type != parse_block) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unexpected \"}\"");
                goto failed;
            }

            goto done;
        }

        if (rc == NGX_CONF_FILE_DONE) {

            if (type == parse_block) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "unexpected end of file, expecting \"}\"");
                goto failed;
            }

            goto done;
        }

        if (rc == NGX_CONF_BLOCK_START) {

            if (type == parse_param) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "block directives are not supported "
                                   "in -g option");
                goto failed;
            }
        }

        /* rc == NGX_OK || rc == NGX_CONF_BLOCK_START */

        /*
         * 用户自定义的解析块类型指令的方法,这种指令对应的块内中的内容都不是指令。
         *
         * 目前是专门为types这种类型的指令指定定义的,因为types块内的内容不是指令,所以不能像解析一般
         * 块内指令那样,对types的解析内的内容解释有ngx_http_core_module模块自己决定,比如ngx_http_core_type方法。
         *
         *
         * 目前使用的模块有:
         * /src/http/modules/ngx_http_charset_filter_module.c:1286:    cf->handler = ngx_http_charset_map;
		 * /src/http/modules/ngx_http_geo_module.c:456:    cf->handler = ngx_http_geo;
		 * /src/http/modules/ngx_http_map_module.c:273:    cf->handler = ngx_http_map;
		 * /src/http/modules/ngx_http_split_clients_module.c:168:    cf->handler = ngx_http_split_clients;
		 * /src/http/ngx_http_core_module.c:3374:    cf->handler = ngx_http_core_type;
         */
        if (cf->handler) {

            /*
             * the custom handler, i.e., that is used in the http's
             * "types { ... }" directive
             *
             *	types {
             *		{}; // 不允许出现这种情况
             *		text/html html; // 正常的情况
             *	}
             *
             */
            if (rc == NGX_CONF_BLOCK_START) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unexpected \"{\"");
                goto failed;
            }

            /*
             * cf->handler() 这样调用也可以;函数指针的两种不同调用方式
             *
             * 假设pfunc是一个指向函数的指针,*pfunc就是这个函数,按照这种解释,则我们在使用
             * 函数指针调用函数的时候应该使用 *pfunc 来调用函数,像这样
             * 		int c = (*pfunc)(5,6)
             * 历史上,贝尔实验室的C和Unix的开发者使用的就是这种观点,即
             * 		int c = (*pfunc)(5,6)
             * 而Berkeyly的Unix的扩展这采用函数指针的形式对其调用,即
             *		pfunc(5,6)
             * 标准C为了兼容性,两种方式都接受。
             */
            rv = (*cf->handler)(cf, NULL, cf->handler_conf);
            if (rv == NGX_CONF_OK) {
                continue;
            }

            if (rv == NGX_CONF_ERROR) {
                goto failed;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, rv);

            goto failed;
        }


        // 从所有模块中遍历对比解析到的指令,如果匹配则执行
        rc = ngx_conf_handler(cf, rc);

        if (rc == NGX_ERROR) {
            goto failed;
        }
    }

failed:

    rc = NGX_ERROR;

done:

    if (filename) {
        if (cf->conf_file->buffer->start) {
            ngx_free(cf->conf_file->buffer->start);
        }

        if (ngx_close_file(fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
                          ngx_close_file_n " %s failed",
                          filename->data);
            rc = NGX_ERROR;
        }

        cf->conf_file = prev;
    }

    if (rc == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

/*
 * 执行指令绑定的方法
 *
 * 1.遍历所有模块的所有指令和解析到的指令对比
 * 2.如果对比成功则执行该指令对应的方法
 */
static ngx_int_t
ngx_conf_handler(ngx_conf_t *cf, ngx_int_t last)
{
    char           *rv;
    void           *conf, **confp;
    ngx_uint_t      i, found;
    ngx_str_t      *name;
    ngx_command_t  *cmd;

    // 指令名字
    name = cf->args->elts;

    found = 0;

    // 遍历所有模块
    for (i = 0; ngx_modules[i]; i++) {

        cmd = ngx_modules[i]->commands;
        if (cmd == NULL) {
            continue;
        }

        // 遍历某个模块的所有指令
        for ( /* void */ ; cmd->name.len; cmd++) {

        	// 比较指令名字的长度是否一致
            if (name->len != cmd->name.len) {
                continue;
            }

            // 比较指令名字是否一样
            if (ngx_strcmp(name->data, cmd->name.data) != 0) {
                continue;
            }

            // 在某个模块ngx_modules[i]中发现和当前指令同名的指令
            found = 1;

            if (ngx_modules[i]->type != NGX_CONF_MODULE
                && ngx_modules[i]->type != cf->module_type)
            {
            	// 过滤NGX_CONF_MODULE模块
                continue;
            }

            // 走到这里说明在模块ngx_modules[i]中,有和当前指令同名的指令

            /* is the directive's location right ? */
            // 判断指令的位置是否正确,不正确则忽略。cmd->type定义了指令可以出现的地方
            if (!(cmd->type & cf->cmd_type)) {
                continue;
            }

            // 如果指令不是块类型的,并且ngx_conf_read_token()没有返回NGX_OK则指令格式错误
            if (!(cmd->type & NGX_CONF_BLOCK) && last != NGX_OK) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                  "directive \"%s\" is not terminated by \";\"",
                                  name->data);
                return NGX_ERROR;
            }

            /*
             * 如果是块指令,但是ngx_conf_read_token()没有返回NGX_CONF_BLOCK_START,则
             * 指令格式错误。
             *
             * 比如events {}、server{} 指令
             *
             */
            if ((cmd->type & NGX_CONF_BLOCK) && last != NGX_CONF_BLOCK_START) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "directive \"%s\" has no opening \"{\"",
                                   name->data);
                return NGX_ERROR;
            }

            /* is the directive's argument count right ? */

            /*
             * 检查指令后面带的参数个数是否正确。
             * 如果没有指定参数个数,则最多带八个参数。
             */

            if (!(cmd->type & NGX_CONF_ANY)) {

                if (cmd->type & NGX_CONF_FLAG) {

                    if (cf->args->nelts != 2) {
                        goto invalid;
                    }

                } else if (cmd->type & NGX_CONF_1MORE) {

                    if (cf->args->nelts < 2) {
                        goto invalid;
                    }

                } else if (cmd->type & NGX_CONF_2MORE) {

                    if (cf->args->nelts < 3) {
                        goto invalid;
                    }

                } else if (cf->args->nelts > NGX_CONF_MAX_ARGS) {

                    goto invalid;

                } else if (!(cmd->type & argument_number[cf->args->nelts - 1]))
                {
                    goto invalid;
                }
            }

            /* set up the directive's configuration context */

            conf = NULL;

            if (cmd->type & NGX_DIRECT_CONF) {
            	/*
            	 * 此时cf->ctx有/src/core/ngx_cycle.c/ngx_init_cycle方法传递过来;
            	 *
            	 *
            	 * NGX_DIRECT_CONF 有这个标记代表模块的配置信息结构体放在了,cycle->conf_ctx的第二层指针上,
            	 * 比如ngx的核心模块(ngx_core_module),直接从第二层指针上就可以取到这些模块的配置信息结构体
            	 * 目前使用该标志的模块有:
            	 * 		/core/nginx.c/ngx_core_module模块
            	 * 		/core/ngx_regex.c/ngx_regex_module模块
		    	 *		/core/ngx_thread_pool.c/ngx_thread_pool_module模块
		    	 *		/event/ngx_event_openssl.c/ngx_openssl_module模块
		    	 *		/misc/ngx_google_perftools_module.c/ngx_google_perftools_module模块
				 * 以上这几个模块都是核心模块,其他模块都没有使用该标记。
				 *
				 * 下面的操作取出的conf如下图:
				 *   cf->ctx|cycle->conf_ctx
				 *   -----
				 *	 | * |
				 *	 -----
				 *	 \					   conf
				 *	  ---------           -----
            	 *	  | * | ...           | * |
            	 *	  ---------           -----
				 *	   \                 /
				 *	    -----------------
				 *	    |ngx_core_conf_t|
				 *	    -----------------
            	 */
                conf = ((void **) cf->ctx)[ngx_modules[i]->index];

            } else if (cmd->type & NGX_MAIN_CONF) {
            	/*
            	 * 此时cf->ctx有/src/core/ngx_cycle.c/ngx_init_cycle方法传递过来;
            	 *
            	 *
            	 * 该指令类型是存放在配置文件的顶层区域的,比如:
            	 * 	核心事件模块(ngx_events_module)的"events {}"指令;
            	 * 	核心http模块(ngx_http_module)的"http {}"指令;
            	 *
            	 * 没有使用NGX_DIRECT_CONF标记,则代表当前命令所在的模块使用到了cycle->conf_ctx的四层指针。
            	 *
            	 * 下面的操作是取出当前模块在第二层指针的位置,然后在取出该位置的地址,如下图:
            	 * cf->ctx|cycle->conf_ctx  conf
				 *   -----                 -----
				 *	 | * |                 | * |
				 *	 -----                 -----
				 *	   \                   /
				 *	  	-------------------
            	 *	  	| * |   ...   | * |
            	 *	  	-------------------
            	 * 如果ngx_modules[i]->index 的值等于3,那么conf的值就是第二层中第四个星号变量的地址
            	 */
                conf = &(((void **) cf->ctx)[ngx_modules[i]->index]);

            } else if (cf->ctx) {

            	/*
            	 * 如果是事件模块,那么cf->ctx是有/src/event/ngx_event.c/ngx_events_block方法传递过来的;
            	 * 如果是http模块,那么cf->ctx是有/src/http/ngx_http.c/ngx_http_block方法传递过来的;
            	 *
            	 *
            	 * 这一步是取各个非核心模块的自定义结构体指针了,比如
            	 *  事件模块NGX_EVENT_MODULE;
            	 *  http模块NGX_HTTP_MODULE;
            	 *
            	 *
            	 * 1.如果当前是具体的事件模块,那么cf->ctx的内存结构是这样的:
            	 * cycle->conf_ctx
				 *   -----
				 *	 | * |
				 *	 -----
				 *	  \               		    				    cf->ctx
				 *	   --------------------------------------	     -----
				 *	   | * |  ...  |*ngx_events_module.index| 		 | * |
				 *     --------------------------------------		 -----
				 *     				  					 \            /
				 *					   					 --------------
				 *					   				     |     *      |
				 *					   					 --------------
				 *					   	  				   \
				 *					   	   				    ---------------
				 *					   	   				    | * | ... | * | 存放各个事件模块的配置结构体指针
				 *					   	   				    ---------------
				 *											 /			 \
				 *					   	   		------------------	  ------------------
				 *					   	   		|ngx_event_conf_t|	  |ngx_epoll_conf_t|
				 *								------------------	  ------------------
				 *
            	 * 另外,事件模块的cmd->conf都是零,貌似目前只有http模块使用了cmd->conf字段。
            	 * 最终conf会指向具体的ngx_xxx_cont_t结构体。
            	 *
            	 *
            	 * 2.如果当前是核心http模块(ngx_http_module)在执行ngx_http_block()方法,那么cf->ctx的内存结构是这样的:
            	 *  (所以,所有直接在http{}块下的指令,他们的第一个入参cf,都是下面的这种结构)
            	 *  cycle->conf_ctx
				 *   -----
				 *	 | * |
				 *	 -----
				 *	  \        ngx_http_module.index					 	 cf->ctx
				 *	   ---------------										  -----
				 *	   | * | ... | * | ngx_max_module个						  | * |
				 *     ---------------										  -----
				 *    		 		\         ngx_http_conf_ctx_t				/
				 *    		 	   -----------------------------------------------
				 *    		 	   |  **main_conf  |  **srv_conf  |  **loc_conf  |
				 *			 	   -----------------------------------------------
			     * 			    	 /           	      /                      \
			 	 *			-------------		   -------------			   -------------
			 	 *			| * |...| * |		   | * |...| * |			   | * |...| * | 都是ngx_http_max_module个
			 	 *			-------------		   -------------			   -------------
				 *			  |  					 |							 |
				 *  ---------------------------	  ---------------------------	--------------------------
				 *	|ngx_http_core_main_conf_t|	  |ngx_http_core_main_conf_t|	|ngx_http_core_loc_conf_t|
				 *	---------------------------	  ---------------------------	--------------------------
            	 *
            	 *	server{}指令的ngx_http_core_server方法中不会用到conf这个入参,所以这里传递与否都无所谓。
            	 *
            	 *
            	 * 在http模块中:
            	 *  最终confp的值会和ngx_http_conf_ctx_t结构体中的 xxx_conf 指针相等,具体等于哪个,则依赖于cmd->conf的值。
            	 *  最终conf的值就是xxx_conf[ngx_modules[i]->ctx_index],也就是各个自定义的http模块对应的配置信息结构体。
            	 *
            	 *
            	 * 3.如果当前是http核心模块(ngx_http_core_module)在执行ngx_http_core_server()方法,那么cf->ctx的结构如下:
            	 *  (所以,所有直接在server{}块下的指令,他们的第一个入参cf,都是下面的这种结构)
            	 *  "+"边表示的图形代表是同一个结构体
            	 *
            	 *																				cycle->conf_ctx
            	 *																					 -----
            	 *																					 | * |
            	 *																				     -----
            	 * 	 			   		   cf->ctx														\
				 *				  			-----												  ---------------------
				 *				  			| * |												  | * | ... | * | ... | ngx_max_module个
				 *				  			-----												  ---------------------
				 *	 			 			 \	         ngx_http_conf_ctx_t							       \			ngx_http_conf_ctx_t
				 *    						-----------------------------------------------------			   -----------------------------------------------------
				 *    						|   **main_conf   |   **srv_conf   |   **loc_conf   |			   |   **main_conf   |   **srv_conf   |   **loc_conf   |
				 *    						-----------------------------------------------------			   -----------------------------------------------------
				 *		 			 			/				|				  	   |						   |				 |					|
				 *	         	  	---------------	 	 ---------------	 		---------------			 +------------+	  ---------------	 ---------------
				 *	 		  		| * | ... | * |		 | * | ... | * |  			| * | ... | * | 		 |_main_conf_t|	  | * | ... | * |	 | * | ... | * |都是ngx_http_max_module个
				 *					---------------		 ---------------	    	---------------			 +------------+	  ---------------	 ---------------
				 *	      			  |					  / ngx_http_core_srv_conf_t   \												|					|
				 *	 +-------------------------+		----------------------------	--------------------------		 --------------------------		--------------------------
				 *	 |ngx_http_core_main_conf_t|		| * | *ctx |      ...  	   |	|ngx_http_core_loc_conf_t|		 |ngx_http_core_srv_conf_t|		|ngx_http_core_loc_conf_t|
				 *	 +-------------------------+ 	 	----------------------------	--------------------------		 --------------------------		--------------------------
            	 *
            	 */
                confp = *(void **) ((char *) cf->ctx + cmd->conf);
                if (confp) {
                	printf("=========>%s\n",cmd->name.data);
                    conf = confp[ngx_modules[i]->ctx_index];
                }
            }

            // 回调指令绑定的方法
            rv = cmd->set(cf, cmd, conf);

            if (rv == NGX_CONF_OK) {
                return NGX_OK;
            }

            if (rv == NGX_CONF_ERROR) {
                return NGX_ERROR;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"%s\" directive %s", name->data, rv);

            return NGX_ERROR;
        }
    }

    // 匹配到指令name,但没有出现在正确的位置
    if (found) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%s\" directive is not allowed here", name->data);

        return NGX_ERROR;
    }

    // 没有匹配到指令name
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "unknown directive \"%s\"", name->data);

    return NGX_ERROR;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid number of arguments in \"%s\" directive",
                       name->data);

    return NGX_ERROR;
}


/**
 * 开始解析单词,这个单词并不是传统意义上的单词,也可以理解为一个内容块,
 * 比如worker算是一个单词, "abc 465 sfd"双引号中的内容也算一个单词
 *
 * 每成功解析出一个指令就会返回(一个指令会包含一个或多个单词):
 * 	1.对于普通指令,解析到“;”符号(如: use epoll;)
 * 	2.对于带花括号的,解析到"{"符号(如: events {}),剩下的花括号有events绑定发方法来完成
 *
 */
static ngx_int_t
ngx_conf_read_token(ngx_conf_t *cf)
{
    u_char      *start, ch, *src, *dst;
    off_t        file_size;
    size_t       len;
    ssize_t      n, size;
    ngx_uint_t   found, need_space, last_space, sharp_comment, variable;
    ngx_uint_t   quoted, s_quoted, d_quoted, start_line;
    ngx_str_t   *word;
    ngx_buf_t   *b, *dump;

    // 是否解析出一个单词
    found = 0;
    need_space = 0;
    /*
     * 如果该值等于1,表示当前还没有解析到任何可用的单词
     * 如果该值不等于1,表示当前正在解析单词
     */
    last_space = 1;
    // 当前在解析注释行(#)
    sharp_comment = 0;
    // 当前在解析变量单词
    variable = 0;
    // 当前在解析转义字符(以\开头的字符)
    quoted = 0;
    // 当前在解析单引号中的内容
    s_quoted = 0;
    // 当前在解析双引号中的内容
    d_quoted = 0;

    // 初始化解析到单词个数
    cf->args->nelts = 0;
    // 文件内容缓冲区
    b = cf->conf_file->buffer;
    dump = cf->conf_file->dump;
    // 内容开始位置,一个临时变量,记录解析的过程中,每个单词的开始位置
    start = b->pos;
    // 当前解析的文件行数
    start_line = cf->conf_file->line;

    // 文件包含的总字节数
    file_size = ngx_file_size(&cf->conf_file->file.info);

    for ( ;; ) {

    	/*
    	 * 如果pos和last相等,则代表已经解析完缓冲区中的内容,需要从文件中读取新内容了
    	 */
        if (b->pos >= b->last) {

            if (cf->conf_file->file.offset >= file_size) {

                if (cf->args->nelts > 0 || !last_space) {

                    if (cf->conf_file->file.fd == NGX_INVALID_FILE) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                           "unexpected end of parameter, "
                                           "expecting \";\"");
                        return NGX_ERROR;
                    }

                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                  "unexpected end of file, "
                                  "expecting \";\" or \"}\"");
                    return NGX_ERROR;
                }

                return NGX_CONF_FILE_DONE;
            }

            /*
             * 计算缓冲区是否截断了某个单词
             * 比如单词 worker,如果缓冲区在最后几个字节是“wor”,那么即使b->pos等于b->last
             * 那么在当前缓冲区中,是无法解析出worker这个单词的。
             *
             * 所以这里len的值就是“wor”这个字符串的长度3
             */
            len = b->pos - start;

            if (len == NGX_CONF_BUFFER) {
            	// 一个单词不能大于缓冲区的大小
                cf->conf_file->line = start_line;

                if (d_quoted) {
                    ch = '"';

                } else if (s_quoted) {
                    ch = '\'';

                } else {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "too long parameter \"%*s...\" started",
                                       10, start);
                    return NGX_ERROR;
                }

                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "too long parameter, probably "
                                   "missing terminating \"%c\" character", ch);
                return NGX_ERROR;
            }

            /*
             * 如果len大于0,就像前面说的单词“worker”被缓冲区截断了,为了继续把文件中的内容写入缓冲区,
             * 同事又不覆盖掉截断的内容,下面这个还数就是把截断的内容,移动到缓冲区的最前面,然后再把文件
             * 的内容写入到缓冲区中。
             */
            if (len) {
                ngx_memmove(b->start, start, len);
            }

            // 计算未读到缓冲区的文件还有多大
            size = (ssize_t) (file_size - cf->conf_file->file.offset);

            if (size > b->end - (b->start + len)) {
            	// 如果剩余文件的大小,大于缓冲区实际可以写入的大小,则使用缓冲区的大小
                size = b->end - (b->start + len);
            }

            // 读取文件内容到缓冲区
            n = ngx_read_file(&cf->conf_file->file, b->start + len, size,
                              cf->conf_file->file.offset);

            if (n == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (n != size) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   ngx_read_file_n " returned "
                                   "only %z bytes instead of %z",
                                   n, size);
                return NGX_ERROR;
            }

            // 修正缓冲区指针
            b->pos = b->start + len;
            b->last = b->pos + n;
            start = b->start;

            if (dump) {
                dump->last = ngx_cpymem(dump->last, b->pos, size);
            }
        }

        // 解析一个字符
        ch = *b->pos++;

        if (ch == LF) {
        	// 遇见换行符,则行数加一
            cf->conf_file->line++;

            if (sharp_comment) {
                sharp_comment = 0;
            }
        }

        // 当前正在解析注释行
        if (sharp_comment) {
            continue;
        }

        // 当前正在解析转义字符
        if (quoted) {
        	// 转义字符只有两个字符,如\t、\b等
            quoted = 0;
            continue;
        }

        if (need_space) {
            if (ch == ' ' || ch == '\t' || ch == CR || ch == LF) {
                last_space = 1;
                need_space = 0;
                continue;
            }

            if (ch == ';') {
                return NGX_OK;
            }

            if (ch == '{') {
                return NGX_CONF_BLOCK_START;
            }

            if (ch == ')') {
                last_space = 1;
                need_space = 0;

            } else {
                 ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                    "unexpected \"%c\"", ch);
                 return NGX_ERROR;
            }
        }

        /*
         * 如果该值等于1,表示当前还没有解析到任何可用的单词
         * 如果该值不等于1,表示当前正在解析单词
         */
        if (last_space) {
            if (ch == ' ' || ch == '\t' || ch == CR || ch == LF) {
                continue;
            }

            // 到这里表明,解析到了单词的第一个字符
            start = b->pos - 1;
            start_line = cf->conf_file->line;

            switch (ch) {

            case ';':
            case '{':
            	/*
            	 * 如果单词的第一个字符是'{',那么前面一定要有一个指令名字
            	 * 比如"events {",如果没有则肯定是错误的
            	 */
                if (cf->args->nelts == 0) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "unexpected \"%c\"", ch);
                    return NGX_ERROR;
                }

                if (ch == '{') {// 多此一举的判断?
                	/*
                	 * 如果单词的第一个字符是'{',则直接返回
                	 * 花括号的解析交给 '{' 字符前面的指令去解析
                	 */
                    return NGX_CONF_BLOCK_START;
                }

                return NGX_OK;

            case '}':
                if (cf->args->nelts != 0) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "unexpected \"}\"");
                    return NGX_ERROR;
                }

                return NGX_CONF_BLOCK_DONE;

            case '#':
                sharp_comment = 1;
                continue;

            case '\\':
                quoted = 1;
                last_space = 0;
                continue;

            case '"':
                start++;
                d_quoted = 1;
                last_space = 0;
                continue;

            case '\'':
                start++;
                s_quoted = 1;
                last_space = 0;
                continue;

            default:
                last_space = 0;
            }

        } else {
        	/*当前正在解析单词的过程中*/

            if (ch == '{' && variable) {
                continue;
            }

            variable = 0;

            if (ch == '\\') {
                quoted = 1;
                continue;
            }

            if (ch == '$') {
                variable = 1;
                continue;
            }

            if (d_quoted) {
                if (ch == '"') {
                    d_quoted = 0;
                    need_space = 1;
                    found = 1;
                }

            } else if (s_quoted) {
                if (ch == '\'') {
                    s_quoted = 0;
                    need_space = 1;
                    found = 1;
                }

            } else if (ch == ' ' || ch == '\t' || ch == CR || ch == LF
                       || ch == ';' || ch == '{')
            {
                last_space = 1;
                found = 1;
            }

            // 解析出一个单词
            if (found) {
            	// 将解析出的单词放入数组中
                word = ngx_array_push(cf->args);
                if (word == NULL) {
                    return NGX_ERROR;
                }

                // 为数组元素分配空间
                word->data = ngx_pnalloc(cf->pool, b->pos - 1 - start + 1);
                if (word->data == NULL) {
                    return NGX_ERROR;
                }

                // 将缓冲区冲的单词内容,拷贝到数组元素word中
                for (dst = word->data, src = start, len = 0;
                     src < b->pos - 1;
                     len++)
                {
                    if (*src == '\\') {
                        switch (src[1]) {
                        case '"':
                        case '\'':
                        case '\\':

        		    		/*
        		    		 * 如果转义字符是这种形式
        		    		 * name\"cdb  age\'person  cycle\\woker,则直接忽略这个'\'
        		    		 * 不需要将字符'\'赋值到数组中
        		    		 */
                            src++;
                            break;

                        case 't':
        		   			/*
        		   			 * 如果是转义"\t"
        		   			 * 则将字符串形式的"\t",转换成计算语言的'\t'
        		   			 */
                            *dst++ = '\t';
                            src += 2;
                            continue;

                        case 'r':
                            *dst++ = '\r';
                            src += 2;
                            continue;

                        case 'n':
                            *dst++ = '\n';
                            src += 2;
                            continue;
                        }

                    }
                    *dst++ = *src++;
                }

                // 为字符串加一个结束符,这样这个字符串也可以脱离ngx的字符串结构体
                *dst = '\0';
                word->len = len;

                if (ch == ';') {
                	// 单词的下一个符号是';'则表示解析出一个完整指令
                    return NGX_OK;
                }

                if (ch == '{') {
                	// 单词的下一个字符是'{'则表示这是一个以花括号为入参的指令
                    return NGX_CONF_BLOCK_START;
                }

                found = 0;
            }
        }
    }
}


char *
ngx_conf_include(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char        *rv;
    ngx_int_t    n;
    ngx_str_t   *value, file, name;
    ngx_glob_t   gl;

    value = cf->args->elts;
    file = value[1];

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "include %s", file.data);

    if (ngx_conf_full_name(cf->cycle, &file, 1) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (strpbrk((char *) file.data, "*?[") == NULL) {

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "include %s", file.data);

        return ngx_conf_parse(cf, &file);
    }

    ngx_memzero(&gl, sizeof(ngx_glob_t));

    gl.pattern = file.data;
    gl.log = cf->log;
    gl.test = 1;

    if (ngx_open_glob(&gl) != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           ngx_open_glob_n " \"%s\" failed", file.data);
        return NGX_CONF_ERROR;
    }

    rv = NGX_CONF_OK;

    for ( ;; ) {
        n = ngx_read_glob(&gl, &name);

        if (n != NGX_OK) {
            break;
        }

        file.len = name.len++;
        file.data = ngx_pstrdup(cf->pool, &name);
        if (file.data == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "include %s", file.data);

        rv = ngx_conf_parse(cf, &file);

        if (rv != NGX_CONF_OK) {
            break;
        }
    }

    ngx_close_glob(&gl);

    return rv;
}


ngx_int_t
ngx_conf_full_name(ngx_cycle_t *cycle, ngx_str_t *name, ngx_uint_t conf_prefix)
{
    ngx_str_t  *prefix;

    prefix = conf_prefix ? &cycle->conf_prefix : &cycle->prefix;

    return ngx_get_full_name(cycle->pool, prefix, name);
}


ngx_open_file_t *
ngx_conf_open_file(ngx_cycle_t *cycle, ngx_str_t *name)
{
    ngx_str_t         full;
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_open_file_t  *file;

#if (NGX_SUPPRESS_WARN)
    ngx_str_null(&full);
#endif

    if (name->len) {
        full = *name;

        if (ngx_conf_full_name(cycle, &full, 0) != NGX_OK) {
            return NULL;
        }

        part = &cycle->open_files.part;
        file = part->elts;

        for (i = 0; /* void */ ; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }
                part = part->next;
                file = part->elts;
                i = 0;
            }

            if (full.len != file[i].name.len) {
                continue;
            }

            if (ngx_strcmp(full.data, file[i].name.data) == 0) {
            	// 如果文件已经存在则直接返回
                return &file[i];
            }
        }
    }

    file = ngx_list_push(&cycle->open_files);
    if (file == NULL) {
        return NULL;
    }

    if (name->len) {
        file->fd = NGX_INVALID_FILE;
        file->name = full;

    } else {
        file->fd = ngx_stderr;
        file->name = *name;
    }

    file->flush = NULL;
    file->data = NULL;

    return file;
}


static void
ngx_conf_flush_files(ngx_cycle_t *cycle)
{
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_open_file_t  *file;

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, cycle->log, 0, "flush files");

    part = &cycle->open_files.part;
    file = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            file = part->elts;
            i = 0;
        }

        if (file[i].flush) {
            file[i].flush(&file[i], cycle->log);
        }
    }
}


void ngx_cdecl
ngx_conf_log_error(ngx_uint_t level, ngx_conf_t *cf, ngx_err_t err,
    const char *fmt, ...)
{
    u_char   errstr[NGX_MAX_CONF_ERRSTR], *p, *last;
    va_list  args;

    last = errstr + NGX_MAX_CONF_ERRSTR;

    va_start(args, fmt);
    p = ngx_vslprintf(errstr, last, fmt, args);
    va_end(args);

    if (err) {
        p = ngx_log_errno(p, last, err);
    }

    if (cf->conf_file == NULL) {
        ngx_log_error(level, cf->log, 0, "%*s", p - errstr, errstr);
        return;
    }

    if (cf->conf_file->file.fd == NGX_INVALID_FILE) {
        ngx_log_error(level, cf->log, 0, "%*s in command line",
                      p - errstr, errstr);
        return;
    }

    ngx_log_error(level, cf->log, 0, "%*s in %s:%ui",
                  p - errstr, errstr,
                  cf->conf_file->file.name.data, cf->conf_file->line);
}


char *
ngx_conf_set_flag_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_str_t        *value;
    ngx_flag_t       *fp;
    ngx_conf_post_t  *post;

    fp = (ngx_flag_t *) (p + cmd->offset);

    if (*fp != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        *fp = 1;

    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        *fp = 0;

    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                     "invalid value \"%s\" in \"%s\" directive, "
                     "it must be \"on\" or \"off\"",
                     value[1].data, cmd->name.data);
        return NGX_CONF_ERROR;
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, fp);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_str_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_str_t        *field, *value;
    ngx_conf_post_t  *post;

    field = (ngx_str_t *) (p + cmd->offset);

    if (field->data) {
        return "is duplicate";
    }

    value = cf->args->elts;

    *field = value[1];

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, field);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_str_array_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_str_t         *value, *s;
    ngx_array_t      **a;
    ngx_conf_post_t   *post;

    a = (ngx_array_t **) (p + cmd->offset);

    if (*a == NGX_CONF_UNSET_PTR) {
        *a = ngx_array_create(cf->pool, 4, sizeof(ngx_str_t));
        if (*a == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    s = ngx_array_push(*a);
    if (s == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    *s = value[1];

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, s);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_keyval_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_str_t         *value;
    ngx_array_t      **a;
    ngx_keyval_t      *kv;
    ngx_conf_post_t   *post;

    a = (ngx_array_t **) (p + cmd->offset);

    if (*a == NULL) {
        *a = ngx_array_create(cf->pool, 4, sizeof(ngx_keyval_t));
        if (*a == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    kv = ngx_array_push(*a);
    if (kv == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    kv->key = value[1];
    kv->value = value[2];

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, kv);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_num_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_int_t        *np;
    ngx_str_t        *value;
    ngx_conf_post_t  *post;


    np = (ngx_int_t *) (p + cmd->offset);

    if (*np != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;
    *np = ngx_atoi(value[1].data, value[1].len);
    if (*np == NGX_ERROR) {
        return "invalid number";
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, np);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_size_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    size_t           *sp;
    ngx_str_t        *value;
    ngx_conf_post_t  *post;


    sp = (size_t *) (p + cmd->offset);
    if (*sp != NGX_CONF_UNSET_SIZE) {
        return "is duplicate";
    }

    value = cf->args->elts;

    *sp = ngx_parse_size(&value[1]);
    if (*sp == (size_t) NGX_ERROR) {
        return "invalid value";
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, sp);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_off_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    off_t            *op;
    ngx_str_t        *value;
    ngx_conf_post_t  *post;


    op = (off_t *) (p + cmd->offset);
    if (*op != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    *op = ngx_parse_offset(&value[1]);
    if (*op == (off_t) NGX_ERROR) {
        return "invalid value";
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, op);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_msec_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_msec_t       *msp;
    ngx_str_t        *value;
    ngx_conf_post_t  *post;


    msp = (ngx_msec_t *) (p + cmd->offset);
    if (*msp != NGX_CONF_UNSET_MSEC) {
        return "is duplicate";
    }

    value = cf->args->elts;

    *msp = ngx_parse_time(&value[1], 0);
    if (*msp == (ngx_msec_t) NGX_ERROR) {
        return "invalid value";
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, msp);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_sec_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    time_t           *sp;
    ngx_str_t        *value;
    ngx_conf_post_t  *post;


    sp = (time_t *) (p + cmd->offset);
    if (*sp != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    *sp = ngx_parse_time(&value[1], 1);
    if (*sp == (time_t) NGX_ERROR) {
        return "invalid value";
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, sp);
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_bufs_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char *p = conf;

    ngx_str_t   *value;
    ngx_bufs_t  *bufs;


    bufs = (ngx_bufs_t *) (p + cmd->offset);
    if (bufs->num) {
        return "is duplicate";
    }

    value = cf->args->elts;

    bufs->num = ngx_atoi(value[1].data, value[1].len);
    if (bufs->num == NGX_ERROR || bufs->num == 0) {
        return "invalid value";
    }

    bufs->size = ngx_parse_size(&value[2]);
    if (bufs->size == (size_t) NGX_ERROR || bufs->size == 0) {
        return "invalid value";
    }

    return NGX_CONF_OK;
}


char *
ngx_conf_set_enum_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_uint_t       *np, i;
    ngx_str_t        *value;
    ngx_conf_enum_t  *e;

    np = (ngx_uint_t *) (p + cmd->offset);

    if (*np != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    value = cf->args->elts;
    e = cmd->post;

    for (i = 0; e[i].name.len != 0; i++) {
        if (e[i].name.len != value[1].len
            || ngx_strcasecmp(e[i].name.data, value[1].data) != 0)
        {
            continue;
        }

        *np = e[i].value;

        return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "invalid value \"%s\"", value[1].data);

    return NGX_CONF_ERROR;
}


char *
ngx_conf_set_bitmask_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_uint_t          *np, i, m;
    ngx_str_t           *value;
    ngx_conf_bitmask_t  *mask;


    np = (ngx_uint_t *) (p + cmd->offset);
    value = cf->args->elts;
    mask = cmd->post;

    for (i = 1; i < cf->args->nelts; i++) {
        for (m = 0; mask[m].name.len != 0; m++) {

            if (mask[m].name.len != value[i].len
                || ngx_strcasecmp(mask[m].name.data, value[i].data) != 0)
            {
                continue;
            }

            if (*np & mask[m].mask) {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                                   "duplicate value \"%s\"", value[i].data);

            } else {
                *np |= mask[m].mask;
            }

            break;
        }

        if (mask[m].name.len == 0) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "invalid value \"%s\"", value[i].data);

            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


#if 0

char *
ngx_conf_unsupported(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    return "unsupported on this platform";
}

#endif


char *
ngx_conf_deprecated(ngx_conf_t *cf, void *post, void *data)
{
    ngx_conf_deprecated_t  *d = post;

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "the \"%s\" directive is deprecated, "
                       "use the \"%s\" directive instead",
                       d->old_name, d->new_name);

    return NGX_CONF_OK;
}


char *
ngx_conf_check_num_bounds(ngx_conf_t *cf, void *post, void *data)
{
    ngx_conf_num_bounds_t  *bounds = post;
    ngx_int_t  *np = data;

    if (bounds->high == -1) {
        if (*np >= bounds->low) {
            return NGX_CONF_OK;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "value must be equal to or greater than %i",
                           bounds->low);

        return NGX_CONF_ERROR;
    }

    if (*np >= bounds->low && *np <= bounds->high) {
        return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "value must be between %i and %i",
                       bounds->low, bounds->high);

    return NGX_CONF_ERROR;
}
