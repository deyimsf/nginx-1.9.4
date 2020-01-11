
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/*
 * 两个链表:
 *  postponed
 *	posted_requests
 * 第一个链表中存放的是ngx_http_postponed_request_t对象
 * 第二个链表中个存放的是ngx_http_posted_request_t对象
 *
 * 第一链表posted_requests存放主请求下的所有子请求
 * 第二个链表postponed存放每个请求的直接子请求
 *
 * postponed链表某一时刻的状态图:
 * (_req代表请求对象  _postpone代表postponed链表中的链表项  sub1_postpone代表sub1对应的postponed链表项)
 *		   ==============
 *		   | parent_req |
 *		   ==============
 *			   \*postponed
 *		 	 -----------------      -----------------       -----------------
 *		 	 | sub1_postpone | ---> | sub2_postpone | --->  | data_postpone |
 *		 	 -----------------      -----------------       -----------------
 *		 	   \*request			   \*request                 \*out
 *			  +==========+	          ============
 *			  | sub1_req |	          | sub2_req |
 *			  +==========+	          ============
 *					     				 \*postponed
 *					                  -----------------       -----------------
 *					                  | sub3_postpone | --->  | data_postpone |
 *					                  -----------------       -----------------
 *					                      \*request                  \*out
 *					                    ============
 *					                    | sub3_req |
 *								        ============
 *										   \*postponed
 *									    -----------------
 *									    | data_postpone |
 *									    -----------------
 *											  \*out
 *
 * 上面这个图看起来比较费劲,为了方便画图,我们把request和postpone合并一下看做是一个对象,就用subx表示,数据节点单独用data表示
 * 那么上图可以简化成如下:
 * (下图中subx即表示postpone链表项,又表示request对象; data仅表示postpone的数据链表项; parent仅表示reqeust对象)
 *       ----------
 *       | parent |
 *       ----------
 *           \*postpone
 *           +------+      --------      --------
 *           | sub1 | ---> | sub2 | ---> | data |
 *           +------+      --------      --------
 *                           \*postpone
 *                           --------      --------
 *                           | sub3 | ---> | data |
 *  						 --------      --------
 *  						    \
 *  						   --------
 *  						   | data |
 *   						   --------
 * 如此一来这个图形就简单了许多
 *
 * 其中sub1表示该他输出数据了,也就是此时的c->data。
 * 如果在整个请求过程中不会再产生子请求了,那么上图数据的输出顺序如下:
 *   sub1产生的数据 --> sub3下的data --> sub3后的data --> sub2后的data
 * c->data始终代当前事件要执行的请求,所以说当事件到来后,c->data中存放的是哪个请求,那就触发哪个请求的写事件方法
 *
 * 除了用c->data的方式触发请求执行,另一个方式是使用ngx_http_run_posted_requests()方法这个方法的作用是遍历
 * r->main->posted_requests链表中的子请求,然后依次调用对应子请求的写事件方法(write_event_handler)
 *
 * 基本上用ngx_http_run_posted_requests()方法的作用是快速触发子请求,比如一个主请求的事件中一次发起5个人子请求
 * 那么这5个子请求会都放到r->main->posted_requests链中,在本次事件的末尾调用一下该方法,然后这5个子请求就都触发了
 * 如果这5个子请求都是需要从upstream中获取数据的,那么我们就可以并行的接收子请求的数据
 *
 * 当然如果不用他就只能用c->data的方法一个一个的触发了,那么用c->data的效率就比较低了,因为它是有序发起子请求的,也就
 * 是一个子请求返回所有数据后再发起另一个.
 *
 *
 * ngx_http_finalize_request()方法中的有处理子请求的逻辑,它会向上移动c->data值,比如当sub1数据输出完毕
 * 后把c->data设置为parent,后续该过滤器会通过parent->postponed的判断来执行并设置c->data为sub2
 *
 *
 * 方法/src/http/ngx_http_request.c/ngx_http_post_request()负责把子请求放入到r->main->posted_requests链表中
 * 用到ngx_http_post_request()方法的地方有:
 * 	  /src/http/ngx_http_core_module.c/ngx_http_subrequest()
 * 	  /src/http/ngx_http_postponed_filter_module.c
 * 等。
 *
 *
 * 在http的整个请求过程中c->data在调用ngx_http_wait_request_handler()方法后,就一直表示可以向客户端输出
 * 数据的请求对象。
 * _request_handler()方法会调用ngx_http_create_request()创建主请求,并设置c->data为主请求。
 *
 *
 * 在ngx中有些子请求时不允许嵌套的:
 * 	The ngx.location.capture and ngx.location.capture_multi directives cannot capture locations that include
 * 	the add_before_body, add_after_body, auth_request, echo_location, echo_location_async, echo_subrequest,
 * 	or echo_subrequest_async directives.
 * 其中ngx.location.capture and ngx.location.capture_multi这两个指令允许互相嵌套,但是其它的不允许
 *
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static ngx_int_t ngx_http_postpone_filter_add(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_postpone_filter_init(ngx_conf_t *cf);


static ngx_http_module_t  ngx_http_postpone_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_postpone_filter_init,         /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_postpone_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_postpone_filter_module_ctx,  /* module context */
    NULL,                                  /* module directives */
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


static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


/*
 * 处理子请求的过滤器
 */
static ngx_int_t
ngx_http_postpone_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_connection_t              *c;
    ngx_http_postponed_request_t  *pr;

    c = r->connection;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http postpone filter \"%V?%V\" %p", &r->uri, &r->args, in);

    /*
     * c->data中存放的是可以向客户端输出数据的请求
     * 在启动其请求的/src/http/ngx_http_core_module.c/ngx_http_subrequest()方法中会设置
     */

    if (r != c->data) {

    	/*
    	 * 走到这里说明还没有轮到请求r来向客户端发数据,此时r->postponed的状态图可能如下:
    	 * 图1:
    	 * 			   ----------
    	 * 			   | parent |
    	 * 			   ----------
    	 * 			      \*postponed
    	 * 			      +------+      --------	 --------     --------
    	 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
    	 * 			      +------+		--------	 --------     --------
    	 * 			      					\			 \
    	 * 			      				   --------    --------
    	 * 			      				   | data |    | data |
    	 * 			      				   --------    --------
    	 * 此时当前请求可能是除了sub1的任何一个子请求节点,比如r==sub2
    	 */

        if (in) {

        	/*
        	 * 如果r是图1中的parent,下面的方法会把in中的buf数据移动到sub3后面的data节点中
        	 *
        	 * 如果r是图1中的sub2,会把in中的buf数据移动到sub2的下面
        	 *
        	 * 如果r是图1中的sub3,会把in中的buf数据移动到sub3的下面
        	 *
        	 *
        	 * 当前请求并不是活跃请求，所以不能直接将数据输出到客户端，只能讲数据暂存起来
        	 * 该方法会把本次要向外输出的数据in追加到r->postponed列表末尾
        	 */
            ngx_http_postpone_filter_add(r, in);

            // 将数据暂存起来后就直接返回

            return NGX_OK;
        }

#if 0
        /* TODO: SSI may pass NULL */
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "http postpone filter NULL inactive request");
#endif

        return NGX_OK;
    }

    /*
     * *************** 能走到这里说明r == c->data ***********************
     * 代表该当前请求向浏览器输出数据了,那么具体该请求能不能向外输出数据
     * 还需要看当前请求中有没有子请求,如果没有就直接输出,有的话就需要先把数据暂存起来
     *
     * 走到这里后可能的图形如下:
     *
     * 图2:
     * 			   ----------
     * 			   | parent |
     * 			   ----------
     * 			      \*postponed
     * 			      +------+      --------	 --------     --------
     * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
     * 			      +------+		--------	 --------     --------
     * 			      					\			 \
     * 			      				   --------    --------
     * 			      				   | data |    | data |
     * 			      				   --------    --------
     * 此时c->data == sub1->request, r == sub1->request
     *
     *
     * 图:3
	 * 			   				+--------+
	 * 			   				| parent |
	 * 			   				+--------+
	 * 			     				\*postponed
	 * 			      --------      --------	 --------     --------
	 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
	 * 			      --------		--------	 --------     --------
	 * 			      					\			 \
	 * 			      				   --------    --------
	 * 			      				   | data |    | data |
	 * 			      				   --------    --------
	 * 此时c->data == parent, r == parent
	 *
	 *
	 * 图:4
	 * 			   			   					----------
	 * 			   			   					| parent |
	 * 			   			   					----------
	 * 			     				 			  \*postponed
	 * 			      --------      +------+	 --------     --------
	 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
	 * 			      --------		+------+	 --------     --------
	 * 			      					\			 \
	 * 			      				 --------    	--------
	 * 			      				 | data |    	| data |
	 * 			      				 --------    	--------
	 * 此时c->data == sub2->request, r == sub2->request
	 *
     */

    if (r->postponed == NULL) {
    	/*
    	 * 如果r->postponed等于NULL则,r->parent->postponed的图形可能如图2
    	 *
    	 * r->postponed为空代表,当前请求中已经不存在子请求了,所以可以把数据直接输出到客户端浏览器了
    	 * 也就是说可以直接向主请求(r->main)输出响应数据了
    	 */
        if (in || c->buffered) {
        	/*
        	 * 如果还有数据没有输出,则再次调用后续的过滤器,将本子请求产生的数据输出到主请求(r->main)的客户端中
             *
        	 * 就像图2中展示的那样,如果有数据就直接输出到r->main中就行了
        	 *
        	 * copy过滤器会不停的触发当前的过滤器,除非无法把数据一次性的输出出去,这时候会返回NGX_AGIAN,否则copy是不
        	 * 会返回的,会一直循环ctx->in中的数据进行输出
        	 *
        	 * 当copy返回后才会调用ngx_http_finalize_request()方法中的逻辑
        	 *
        	 * 因为此时c->data == sub1->request,所以只要sub1的数据没有输出完毕,每次事件过来后执行的都是sub->reqeust
        	 * 中的写事件方法
        	 */
            return ngx_http_next_body_filter(r->main, in);
        }

        // 没有要输出的数据就直接返回了

        return NGX_OK;
    }

    /*
     * 走到这里说明r->postponed != NULL,也就是说虽然该当前请求r输出数据了,但是r下面还存在子请求,比如图3或图4
     * 所以应该先将该请求的数据暂存起来,然后再找出事实上可以输出数据的子请求,当然如果in为空则继续向下走
     *
     * 1.当前是活跃请求，并且当前请求有数据要输出
     * 2.但当前请求还存在子请求列表，说明这个数据是在这个请求列表之后产生的，所以，需要把要输出的数据追加到r->postponed列表最后
     */
    if (in) {
        ngx_http_postpone_filter_add(r, in);
    }

    /*
     * 下面这个循环的作用:
     *  1.把当前请求的r->postponed链表中第一个非数据节点,放入到r->main->posted_requests链表中
     *    后续会执行这个非数据节点
     *  2.把当前请求的r->postponed链表中连续的数据节点(遇到非数据节点则执行第一步)输出到客户端
     *
     * 此时r->postponed的状态图可能如上面的图3,也可能是上面的图4,即
     * 图:3
	 * 			   				+--------+
	 * 			   				| parent |
	 * 			   				+--------+
	 * 			     				\*postponed
	 * 			      --------      --------	 --------     --------
	 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
	 * 			      --------		--------	 --------     --------
	 * 			      					\			 \
	 * 			      				   --------    --------
	 * 			      				   | data |    | data |
	 * 			      				   --------    --------
     * 此时c->data == parent  r == parent, 并且 r->postponed != NULL
     *
     *
     * 图:4
	 * 			   			   					----------
	 * 			   			   					| parent |
	 * 			   			   					----------
	 * 			     				 			  \*postponed
	 * 			      --------      +------+	 --------     --------
	 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
	 * 			      --------		+------+	 --------     --------
	 * 			      					\			 \
	 * 			      				 --------    	--------
	 * 			      				 | data |    	| data |
	 * 			      				 --------    	--------
	 * 此时c->data == sub2->request, r == sub2->request, 并且 r->postponed != NULL
     */
    do {
        pr = r->postponed;

        /**
         * 这是一个请求节点
         */
        if (pr->request) {

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter wake \"%V?%V\"",
                           &pr->request->uri, &pr->request->args);

            /*
             * 因为pr->request是有值的,说明pr是一个请求节点,所以这里postponed图形只能是上面的图3
			 * 图:3
			 * 			   				+--------+
			 * 			   				| parent |
			 * 			   				+--------+
			 * 			     				 \*postponed
			 * 			      --------      --------	 --------     --------
			 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
			 * 			      --------		--------	 --------     --------
			 * 			      					\			 \
			 * 			      				   --------    --------
			 * 			      				   | data |    | data |
			 * 			      				   --------    --------
			 * 此时c->data == parent, r == parent, 并且 r->postponed != NULL
             */

            r->postponed = pr->next;

            /*
             * 当前请求下还存在子请求,应该先输出子请求的数据才能输出当前请求中的数据,所以c->data
             * 设置为该子请求(pr->reqeust)
             */
            c->data = pr->request;

            /*
             * 此时当上面的两句代码执行完毕后,图3就变成了如下图:
			 * 图5:
			 * 			   							----------
			 * 			   							| parent |
			 * 			   							----------
			 * 			     				 			  \*postponed
			 * 			      --------      +------+	 --------     --------
			 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
			 * 			      --------		+------+	 --------     --------
			 * 			      					\			 \
			 * 			      				   --------    --------
			 * 			      				   | data |    | data |
			 * 			      				   --------    --------
			 * 此时c->data == sub2->request, r == parent
			 *
             * 后续需要做的是把sub2中的数据节点输出到r->main中,所以调用了ngx_http_post_request()方法
             * 来触发sub2这个子请求的写事件执行
             */

            /*
             * 把子请求(pr->request)放入到r->main->posted_request链表中
             *
             * 当下面这个方法返回后会执行ngx_http_finalize_request()方法,该方法会调用
             * ngx_http_set_write_handler(r)将sub2的写事件注册为ngx_http_writer
             *
             * 当前写事件执行完毕后会立即执行ngx_http_run_posted_requests()方法,从而触发sub2->request
             * 写事件的执行,sub2->request的写事件方法ngx_http_writer
             */
            return ngx_http_post_request(pr->request, NULL);
        }

        /*
         * 走到这里说明pr->request == NULL,那此时r->postponed的图形就只能是图4:
		 * 图:4
		 * 			   			   					----------
		 * 			   			   					| parent |
		 * 			   			   					----------
		 * 			     				 			  \*postponed
		 * 			      --------      +------+	 --------     --------
		 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
		 * 			      --------		+------+	 --------     --------
		 * 			      				  \*postponed	 \
		 * 			      				 --------    	--------
		 * 			      				 | data |    	| data |
		 * 			      				 --------    	--------
		 * 此时c->data == sub2->request, r == sub2->request
         */

        if (pr->out == NULL) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "http postpone filter NULL output");

        } else {
        	/**
        	 * 这是一个数据节点
        	 */

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter output \"%V?%V\"",
                           &r->uri, &r->args);

            /*
             * 把该数据节点的数据输出到客户端(主请求代表的客户端)
             */
            if (ngx_http_next_body_filter(r->main, pr->out) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

        /*
         * 跳过这个数据节点,继续向下遍历
         */
        r->postponed = pr->next;

        /*
         * 上面代码执行完毕后,图4会变成如下形式:
         *
		 * 图:6
		 * 			   			   					----------
		 * 			   			   					| parent |
		 * 			   			   					----------
		 * 			     				 			  \*postponed
		 * 			      --------      +------+	 --------     --------
		 * 			      | sub1 | ---> | sub2 | --> | sub3 | --> | data |
		 * 			      --------		+------+	 --------     --------
		 * 			      				  \*postponed	 \
		 * 			      				     		    --------
		 * 			      				 			   	| data |
		 * 			      				 			   	--------
		 * 此时c->data == sub2->request, r == sub2->request
		 * 可以看到sub2下面的data数据消失了,也就是说sub2->postponed==NULL
		 * 然后就会跳出循环,后续会保持这种图形进入到ngx_http_finalize_request()方法
         */
    } while (r->postponed);

    // 不再调用后续过滤器,直接返回

    return NGX_OK;
}


/*
 * 该方法的作用是把链表in中的数据(buf),移动到链表r->postponed的尾部的数据节点中
 * 如果存在数据节点则创建一个在移动数据
 */
static ngx_int_t
ngx_http_postpone_filter_add(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_postponed_request_t  *pr, **ppr;

    /*
	 * 如果当前请求存在postponed链表,则找出链表的尾部,如果尾部pr->request == NULL
	 * 那么说明该pr就是数据节点,直接将链表in中的数据buf追加到pr->out链表中即可
	 *
	 * 如果当前请求不存在postponed链表,则创建一个pr,然后将pr->request设置为NULL,也
	 * 是说把新创建的pr当做是一个数据节点,然后再把in中的数据buf追加到pr->out中即可
	 */

    if (r->postponed) {

    	// 1.找出当前请求列表中的最后一个节点
        for (pr = r->postponed; pr->next; pr = pr->next) { /* void */ }

        // 2.如果最后一个节点不是子请求，则认为这是一个数据节点
        if (pr->request == NULL) {
        	// 发现数据节点
            goto found;
        }

        // 最后一个不是数据节点，则后续逻辑会创建一个数据节点
        ppr = &pr->next;

    } else {
    	// 不存在数据节点或子请求,则后续逻辑会创建一个数据节点
        ppr = &r->postponed;
    }

    /*
     * 不存在数据节点,所以这里需要自己创建一个数据节点
     */
    pr = ngx_palloc(r->pool, sizeof(ngx_http_postponed_request_t));
    if (pr == NULL) {
        return NGX_ERROR;
    }

    *ppr = pr;

    pr->request = NULL;
    pr->out = NULL;
    pr->next = NULL;

found:

	/*
	 * 将链表in中的buf数据移动到pr->out链表中
	 */
    if (ngx_chain_add_copy(r->pool, &pr->out, in) == NGX_OK) {
        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_postpone_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_postpone_filter;

    return NGX_OK;
}
