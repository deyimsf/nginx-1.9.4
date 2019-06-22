#include <ngx_config.h>  
#include <ngx_core.h>
#include <ngx_http.h>


static char * ngx_http_my_show(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_show_handler(ngx_http_request_t *r);
static void * ngx_http_my_show_create_loc_conf (ngx_conf_t *cf);


typedef struct {
    ngx_str_t  show;
} ngx_http_my_show_local_conf;


static ngx_command_t  ngx_http_my_show_commands[] = {
    { ngx_string("my_show"),
      NGX_HTTP_LOC_CONF| NGX_CONF_TAKE1,
      ngx_http_my_show,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
      
     ngx_null_command
};


static ngx_http_module_t  ngx_http_my_show_module_ctx = {  
    NULL,
    NULL,
   
    NULL,
    NULL,

    NULL,
    NULL,

    ngx_http_my_show_create_loc_conf,
    char      NULL
};


ngx_module_t  ngx_http_my_show_module = {
    NGX_MODULE_V1,
    &ngx_http_my_show_module_ctx,    /* module context */
    ngx_http_my_show_commands,       /* module directives */
    NGX_HTTP_MODULE,                 /* module type */
    NULL,                            /* init master */
    NULL,                            /* init module */
    NULL,                            /* init process */
    NULL,                            /* init thread */
    NULL,                            /* exit thread */
    NULL,                            /* exit process */
    NULL,                            /* exit master */
    NGX_MODULE_V1_PADDING
};


static char * 
ngx_http_my_show(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
   ngx_http_core_loc_conf_t  *clcf;
   ngx_http_my_show_local_conf  *mslf;
   ngx_str_t  *value;

   mslf = conf;

   value = cf->args->elts;
   mslf->show = value[1];

   clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
   clcf->handler = ngx_http_show_handler;

   return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_show_handler(ngx_http_request_t *r)
{
    ngx_http_my_show_local_conf  *mslf;
    ngx_http_complex_value_t  *val;

    val = ngx_pcalloc(r->pool, sizeof(ngx_http_complex_value_t));
    if (val == NULL) {
       return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    mslf = ngx_http_get_module_loc_conf(r, ngx_http_my_show_module);
    val->value = mslf->show;

    return ngx_http_send_response(r, 200, NULL, val);
}


static void *
ngx_http_my_show_create_loc_conf (ngx_conf_t *cf)
{
    ngx_http_my_show_local_conf   *mslf;
     
    mslf = ngx_pcalloc(cf->pool, sizeof(ngx_http_my_show_local_conf));

    return mslf;
}
