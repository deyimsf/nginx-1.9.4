#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_int_t ngx_http_my_init(ngx_conf_t *cf);
static void *ngx_http_my_create_loc_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_my_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_my_body_filter(ngx_http_request_t *r, ngx_chain_t *in);

// 要输出的数据
static ngx_str_t out_data_str = ngx_string("hello world");

// 保存模块指令结构
typedef struct {
    ngx_str_t    my_filter;
} ngx_http_my_loc_conf_t;

// 指令
static ngx_command_t ngx_http_my_commands[]={
        { ngx_string("my_filter"),
          NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
          ngx_conf_set_str_slot, // 这个方法填充的指令是不允许重复的，也就是数不能在一个location中配置两次该指令
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_my_loc_conf_t,my_filter),
          NULL
        },

        ngx_null_command
};

//上下文
static ngx_http_module_t ngx_http_my_filter_module_ctx={
        NULL,
        ngx_http_my_init,

        NULL,
        NULL,

        NULL,
        NULL,

        ngx_http_my_create_loc_conf,
        NULL
};

//模块信息
ngx_module_t ngx_http_my_filter_module = {
        NGX_MODULE_V1,
        &ngx_http_my_filter_module_ctx,
        ngx_http_my_commands,
        NGX_HTTP_MODULE,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NGX_MODULE_V1_PADDING
};

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt   ngx_http_next_body_filter;


//注册过滤器
static ngx_int_t
ngx_http_my_init(ngx_conf_t *cf)
{
	    ngx_http_next_header_filter = ngx_http_top_header_filter;
	    ngx_http_top_header_filter = ngx_http_my_header_filter;

        ngx_http_next_body_filter = ngx_http_top_body_filter;
        ngx_http_top_body_filter = ngx_http_my_body_filter;

        return NGX_OK;
}

// 创建存放指令的结构体
static void *
ngx_http_my_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_my_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool,sizeof(ngx_http_my_loc_conf_t));
    if(conf == NULL){
        return NULL;
    }

    return conf;
}

// 处理响应头
static ngx_int_t
ngx_http_my_header_filter(ngx_http_request_t *r)
{
        ngx_http_my_loc_conf_t *conf;

        //如果响应状态码不是200,或者不是主请求,则不做任何事
        if (r->headers_out.status != NGX_HTTP_OK || r != r->main){
            return ngx_http_next_header_filter(r);
        }

        //拿出loc_conf
        conf = ngx_http_get_module_loc_conf(r, ngx_http_my_filter_module);

        if(conf->my_filter.len == 0){ // 没有配置,则不做任何事
            return ngx_http_next_header_filter(r);
        }

        //走到这里说明配置了该指令,因为我们会向响应结果过中添加数据
        //所以输出长度会改变,这里需要清除content_length头,nginx后续会使用chunked编码来输出数据
        ngx_http_clear_content_length(r);

    return ngx_http_next_header_filter(r);
}

//过滤体
static ngx_int_t
ngx_http_my_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
	ngx_buf_t                       *tail_buf;
	ngx_chain_t                     *tail_chain;
	ngx_http_my_loc_conf_t          *conf;
	ngx_uint_t                      last;
	ngx_chain_t                     *cl;
	// u_char						*tmp;

	//如果要输出的链是NULL,或者客户端只要求输出响应头,或者不是主请求,则不执行过滤逻辑
	if (in == NULL || r->header_only || r != r->main) {
		return ngx_http_next_body_filter(r, in);
	}

	//拿出loc_conf,检查是否配置了指令
	conf = ngx_http_get_module_loc_conf(r, ngx_http_my_filter_module);

	if(conf->my_filter.len == 0){ // 没有配置,则不做任何事
		return ngx_http_next_body_filter(r);
	}

	// 检查是否是请求群中最后一块buf数据
	for (cl = in; cl; cl = cl->next) {
		if (cl->buf->last_buf) {
			cl->buf->last_buf = 0;
			cl->buf->last_in_chain = 0;
			last = 1;
			break; // 此时，cl是in链中最后一个
		}
	}

    //如果不是最后一块则直接输出
    if(!last){
        return ngx_http_next_body_filter(r,in);
    }

    // 如果是的话就创建自己的chain和buf
    tail_buf = ngx_calloc_buf(r->pool);
    if(tail_buf == NULL){
        return NGX_ERROR;
    }

    // 创建自己的chain
    tail_chain = ngx_alloc_chain_link(r->pool);
    if(tail_chain == NULL) {
        return NGX_ERROR;
    }

    // 申请一块内存，并把out_data_str中数据复制过去
    // u_char* tmp = ngx_pcalloc(r->pool, out_data_str.len);
    // ngx_sprintf(tmp,"%s",out_data_str.data);

    // tail_buf指向要输出的数据
    tail_buf->pos = out_data_str.data;
    tail_buf->last = out_data_str.data + out_data_str.len;
    tail_buf->start = tail_buf->pos;
    tail_buf->end = tail_buf->last;
    tail_buf->last_buf = 1;
    tail_buf->last_in_chain = 1;
    tail_buf->memory = 1; // 不能改变这块内存的数据,因为out_data_str是一个全局的

    //把tail_chain、tial_buf关联起来，并不tail_chain追加到in最后
    tail_chain->buf = tail_buf;
    tail_chain->next = NULL;
    cl->next = tail_chain;

    return ngx_http_next_body_filter(r,in);
}
