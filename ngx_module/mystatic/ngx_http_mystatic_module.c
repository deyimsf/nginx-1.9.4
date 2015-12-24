#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

// 处理文件的handler
static ngx_int_t ngx_http_mystatic_handler(ngx_http_request_t *t);
// 解析mystatic指令的函数
static char * ngx_http_mystatic(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);


// 该模块拥有的指令
static ngx_command_t ngx_http_mystatic_commands[] = {
		{
				ngx_string("mystatic"),
				NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
				ngx_http_mystatic,
				NULL,
				NULL,
				NULL
		}
};


// http模块接口
static ngx_http_module_t ngx_http_mystatic_module_ctx = {
		NULL,
		NULL,

		NULL,
		NULL,

		NULL,
		NULL,

		NULL,
		NULL
};


// 声明一个模块
ngx_module_t ngx_http_mystatic_module = {
		NGX_MODULE_V1,
		&ngx_http_mystatic_module_ctx,
		ngx_http_mystatic_commands,
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


// 解析指令
static char *
ngx_http_mystatic(ngx_conf_t *cf, ngx_command_t *cmd, void *conf){
	ngx_http_core_loc_conf_t	*clcf;

	clcf = ngx_http_conf_get_module_loc_conf(cf,ngx_http_core_module);
	clcf->handler = ngx_http_mystatic_handler;

	return NGX_CONF_OK;
}


//处理请求 发送响应
static ngx_int_t
ngx_http_mystatic_handler(ngx_http_request_t *r){
	ngx_int_t rc;

	if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
		// 因为我们只发送静态文件，所有只接受GET和HEAD方法
		return NGX_HTTP_NOT_ALLOWED;
	}

	//丢弃请求包体
	rc = ngx_http_discard_request_body(r);
	if (rc != NGX_OK) {
		return rc;
	}

	ngx_buf_t *b = ngx_palloc(r->pool, sizeof(ngx_buf_t));

	if (b == NULL) {
		return NGX_ERROR;
	}

	// 输出这个文件
	u_char *filename = (u_char*)"/My/test.txt";
	b->in_file = 1;

	//分配一个ngx_file_t用来描述文件
	b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
	b->file->fd = ngx_open_file(filename, NGX_FILE_RDONLY|NGX_FILE_NONBLOCK, NGX_FILE_OPEN, 0);
	b->file->log = r->connection->log;
	b->file->name.data = filename;
	b->file->name.len = strlen(filename);

	if (b->file->fd <= 0) {
		return NGX_HTTP_NOT_FOUND;
	}

	// 为了发送这个文件，需要知道文件的大小，以便在响应头中指定文件内容大小
	if (ngx_file_info(filename, &(b->file->info)) == NGX_FILE_ERROR) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	r->headers_out.content_length_n = b->file->info.st_size;
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_type = (ngx_str_t)ngx_string("text/plain");

	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
		return rc;
	}

	// 指定从文件的什么位置发送数据
	b->file_pos = 0;
	b->file_last = b->file->info.st_size;
	b->last_buf = 1;
	b->last_in_chain = 1;  //这个和上面的last_buf有啥区别?

	ngx_chain_t out;
	out.buf = b;
	out.next = NULL;

	// 清理文件句柄
	// ngx_pool_cleanup_t c = ngx_palloc(p, sizeof(ngx_pool_cleanup_t));
	// c->data = ngx_palloc(p, size);
	ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, sizeof(ngx_pool_cleanup_file_t));
	ngx_pool_cleanup_file_t *clnf = cln->data;

	clnf->fd = b->file->fd;
	clnf->name = b->file->name.data;
	clnf->log = r->pool->log;

	return ngx_http_output_filter(r, &out);
}



