#include <ngx_config.h>  
#include <ngx_core.h>


static void * ngx_my_core_create_conf(ngx_cycle_t *cycle);
static char * ngx_my_core_init_conf(ngx_cycle_t *cycle, void *conf);
static char * ngx_my_core_simple(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * ngx_my_core_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * ngx_my_core_block_handler (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);


static ngx_command_t  ngx_my_core_commands[] = {
   
    { ngx_string("my_core_simple"),
      NGX_MAIN_CONF| NGX_CONF_TAKE1,
      ngx_my_core_simple,
      0,
      0,
      NULL },

    { ngx_string("my_core_block"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_my_core_block,
      0,
      0,
      NULL },

    ngx_null_command
};


static ngx_core_module_t  ngx_my_core_module_ctx = {
    ngx_string("my_core"),
    ngx_my_core_create_conf,
    ngx_my_core_init_conf
};


ngx_module_t  ngx_my_core_module = {
    NGX_MODULE_V1,
    &ngx_my_core_module_ctx,         /* module context */
    ngx_my_core_commands,            /* module directives */
    NGX_CORE_MODULE,                 /* module type */
    NULL,                            /* init master */
    NULL,                            /* init module */
    NULL,                            /* init process */
    NULL,                            /* init thread */
    NULL,                            /* exit thread */
    NULL,                            /* exit process */
    NULL,                            /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_my_core_create_conf(ngx_cycle_t *cycle)
{
   ngx_log_stderr(0, "start runing ngx_my_core_create_conf()");

   return cycle;
}

  
static char *
ngx_my_core_init_conf(ngx_cycle_t *cycle, void *conf)
{
   ngx_log_stderr(0, "start runing ngx_my_core_init_conf()");
   return NULL;
}


static char * 
ngx_my_core_simple(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
   ngx_str_t  *name;

   name = cf->args->elts;
   ngx_log_stderr(0, "find a directive name[%s] value[%s] method[ngx_my_core_simple]",
   				 					name->data, name[1].data);

   return NGX_CONF_OK;
}


static char * 
ngx_my_core_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
   char        *rv;
   ngx_conf_t   save;
   ngx_str_t  *name;

   name = cf->args->elts;

   ngx_log_stderr(0, "find a directive block name[%s] method[ngx_my_core_block]",
        						name->data);

   save = *cf;
   cf->handler = ngx_my_core_block_handler;
   cf->handler_conf = conf;
   
   rv = ngx_conf_parse(cf, NULL);
   
   *cf = save;

   return rv;
}


static char * 
ngx_my_core_block_handler (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
   ngx_uint_t  num;
   ngx_str_t   *words;

   num = cf->args->nelts;

   words = cf->args->elts;

   ngx_log_stderr(0, "find a directive string, num of words[%ui] first word[%s] end word[%s]",
   								 num, words[0].data, words[num-1].data);

   return NGX_CONF_OK;
}

