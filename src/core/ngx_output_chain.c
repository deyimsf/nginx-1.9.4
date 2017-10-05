
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


/*
 * linux中的aio在2.6.22后才支持,并且需要开启directio
 *
 * 当在linux中同时开启aio(directio也必须开启)和sendfile的时候,ngx优先使用aio的方式去读文件数据,否则的会直接sendfile发送数据
 *
 * 在linux中,如果只开启aio指令没有开启directio,那就不会用aio的方式去读文件数居 (TODO 不确定开启aio后是否自动开启directio)
 *
 * 在linux中,如果只开启directio没有开启aio,那么会以阻塞的方式直接从磁盘读数据 (TODO 是这样吗?)
 *
 *
 *
 * 一般情况下文件的读写,应用程序都只跟page cache打交道,当未命中cache的时候由底层去更新page cache,之后
 *	用户在跟page cache打交道
 *
 * direct-io的文件读写形式是,用户直接跟磁盘打交道,cache这一层有用户自己在用户态负责
 *
 * linux中的aio需要directio为打开状态,因为在linux中,如果不是用directio,那么应用程序是只跟page cache
 * 打交道的,也就是直接跟内存(page cache)玩,所以这种状态根本不需要异步操作; 但是当打开directio选项后,就是
 * 用户直接跟磁盘打交道了,这个时候当用户跟磁盘读写数据时开启异步io(aio)就非常有必要了
 *
 *
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#if 0
#define NGX_SENDFILE_LIMIT  4096
#endif

/*
 * When DIRECTIO is enabled FreeBSD, Solaris, and MacOSX read directly
 * to an application memory from a device if parameters are aligned
 * to device sector boundary (512 bytes).  They fallback to usual read
 * operation if the parameters are not aligned.
 * Linux allows DIRECTIO only if the parameters are aligned to a filesystem
 * sector boundary, otherwise it returns EINVAL.  The sector size is
 * usually 512 bytes, however, on XFS it may be 4096 bytes.
 */

#define NGX_NONE            1


static ngx_inline ngx_int_t
    ngx_output_chain_as_is(ngx_output_chain_ctx_t *ctx, ngx_buf_t *buf);
#if (NGX_HAVE_AIO_SENDFILE)
static ngx_int_t ngx_output_chain_aio_setup(ngx_output_chain_ctx_t *ctx,
    ngx_file_t *file);
#endif
static ngx_int_t ngx_output_chain_add_copy(ngx_pool_t *pool,
    ngx_chain_t **chain, ngx_chain_t *in);
static ngx_int_t ngx_output_chain_align_file_buf(ngx_output_chain_ctx_t *ctx,
    off_t bsize);
static ngx_int_t ngx_output_chain_get_buf(ngx_output_chain_ctx_t *ctx,
    off_t bsize);
static ngx_int_t ngx_output_chain_copy_buf(ngx_output_chain_ctx_t *ctx);


/*
 * 利用ctx输出in链表中的数据
 *
 * 如果入参in链表只有一个链表项,并且链表项中的buf可以直接发送出去,那就不使用ctx了,就直接调用下一个过滤器返回了
 *
 * 如果入参in链表中有多个链表项,那么会先将in中所有链表项的buf重新关联到新的链表项,并将其追加到ctx->in中,以后
 * 该方法就一直围绕ctx来处理数据
 *
 * ctx->in: 要发送的数据,该方法会遍历该链表,把链表中的数据组装成一个可以直接发送的链表,用局部变量out表示,
 * 	  如果当前ctx->in->buf可以直接发送出去,那么就直接把当前链表项ctx->in追加到out链的尾部;
 *	  如果当前ctx->in->buf不可以直接发送出去,那么就需要为其创建buf和chain,将数据拷贝到buf中,并将其追加到ut链的尾部;
 *
 *    每形成一个out链就会执行下一个过滤器试着去发送一次数据,不关有没有完全发送完毕都会把out链表项全部移动到ctx->busy链表
 *    下,然后out变量置空并继续装新的链表
 *
 *    ctx->busy链表中空闲的链表项会被释放到ctx->free或pool->chian中
 *
 *
 */
ngx_int_t
ngx_output_chain(ngx_output_chain_ctx_t *ctx, ngx_chain_t *in)
{
    off_t         bsize;
    ngx_int_t     rc, last;
    ngx_chain_t  *cl, *out, **last_out;

    /*
     * ctx->in == NULL有两种情况
     * 		一种是第一次进来该方法,此时还没有把入参in把数据追加到ctx->in中
     *
     * 		另一种是进来n次后已经把ctx->in中的链表项已经完全追加到了out(*last_buf)中,对于需要拷贝的数据
     * 		已经从in中拷贝到了ctx->buf中,并把当时拷贝好的ctx->buf追加到out(*last_buf)中,剩下的就是输
     * 		出out链中的数据
     */
    if (ctx->in == NULL && ctx->busy == NULL
#if (NGX_HAVE_FILE_AIO || NGX_THREADS)
        && !ctx->aio
#endif
       )
    {
        /*
         * the short path for the case when the ctx->in and ctx->busy chains
         * are empty, the incoming chain is empty too or has the single buf
         * that does not require the copy
         */

        if (in == NULL) {

        	/*
        	 * 这种情况表明已经进来该方法多次,并且已经把ctx->in中的链表项已经完全追加到了out(*last_buf)中,
        	 * 对于需要拷贝的数据已经从in中拷贝到了ctx->buf中,并把当时拷贝好的ctx->buf追加到out(*last_buf)中
        	 */
            return ctx->output_filter(ctx->filter_ctx, in);
        }

        if (in->next == NULL
#if (NGX_SENDFILE_LIMIT)
            && !(in->buf->in_file && in->buf->file_last > NGX_SENDFILE_LIMIT)
#endif
            && ngx_output_chain_as_is(ctx, in->buf))
        {
        	/*
        	 * 这种情况只能是第一次进来,此时还没有把入参in把数据追加到ctx->in中,并且in链表中只有一个链表项,并且
        	 * 不关in->buf中的数据是否在文件中,都可以直接发送出去(文件的话支持sendfile),所以这里走一个捷径,不
        	 * 在把入参in加入到ctx->in中去处理,而是直接调用下一个过滤器
        	 */
            return ctx->output_filter(ctx->filter_ctx, in);
        }
    }

    /* add the incoming buf to the chain ctx->in */
    /* 正常路径,把in中的buf追加到ctx->in中 */

    if (in) {

    	/*
    	 * 把入参in中的各个buf追加到ctx->in中,注意这里没有使用in中的ngx_chian_t,而是重新创建的ngx_chain_t
    	 */
        if (ngx_output_chain_add_copy(ctx->pool, &ctx->in, in) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }


    /*
     * 真正要输出的链,ctx->in中的数据最终都会追加到out链中,在out链中的数据是完全可以被直接输出的,out链中的数据消除
     * 了sendfile、aio的特性
     *
     * out和last_out的内存结构如下:
     * 	 last_out|&out
     * 		-----
     * 		| * |
     * 		-----
     *		   \out
     *		   -----
     *		   | * |
     *		   -----
     *
     */
    out = NULL;
    last_out = &out;
    last = NGX_NONE;

    /*
     * 如果可以的话就尽可能的把ctx->in中的链表项追加到out(&last_out)并输出
     */
    for ( ;; ) {

#if (NGX_HAVE_FILE_AIO || NGX_THREADS)
        if (ctx->aio) {
            return NGX_AGAIN;
        }
#endif

        while (ctx->in) {
        	/*
        	 * 遍历ctx->in链表,把每一个链表项中的缓存数据拷贝到ctx->buf中去发送
        	 */

            /*
             * cycle while there are the ctx->in bufs
             * and there are the free output bufs to copy in
             */

            bsize = ngx_buf_size(ctx->in->buf);

            if (bsize == 0 && !ngx_buf_special(ctx->in->buf)) {

                ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                              "zero size buf in output "
                              "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                              ctx->in->buf->temporary,
                              ctx->in->buf->recycled,
                              ctx->in->buf->in_file,
                              ctx->in->buf->start,
                              ctx->in->buf->pos,
                              ctx->in->buf->last,
                              ctx->in->buf->file,
                              ctx->in->buf->file_pos,
                              ctx->in->buf->file_last);

                ngx_debug_point();

                ctx->in = ctx->in->next;

                continue;
            }

            if (ngx_output_chain_as_is(ctx, ctx->in->buf)) {

            	/* move the chain link to the output chain */

            	/*
            	 * 如果当前ctx->in->buf可以直接发送出去,那么就直接把当前链表项in追加到out链的尾部
            	 *
            	 * 可以直接发送有两种情况
            	 * 		一种是buf在内存中
            	 * 		另一种是buf在file中,但是支持sendfile方式,并且没有开启directio
            	 */
                cl = ctx->in;
                // 链表指向下一个链表项
                ctx->in = cl->next;

                /*
                 * 语句 *last_out = cl 执行完后内存结构如下:
            	 *    last_out
				 * 		-----
				 * 		| * |
				 * 		-----
				 *		   \out
				 *		   -----
				 *		   | * |
				 *		   -----
				 *		   	  \cl
				 *		   	 -----
				 *		   	 | * |
				 *		   	 -----
                 */
                *last_out = cl;

                /*
				 * 语句 last_out = &cl->next 执行完后内存结构如下:
				 *		    out
				 *		   -----
				 *		   | * |
				 *		   -----
				 *		   	  \cl		last_out|&cl->next
				 *		   	 -----			-----
				 *		   	 | * |			| * |
				 *		   	 -----			-----
				 *			   \			/
				 *		   	 --------------/-
				 *		   	 | *buf | *next |
				 *		   	 ----------------
				 *		   	 			 \ cl->next
				 *		   	 			 ---------------
				 *		   	 			 | ngx_chain_t |
				 *		   	 			 ---------------
				 *
				 * 从这个图就可以看出last_out始终指向的位置,就是out链表尾部链表项的next变量的地址
				 * 如此反复就可以把在ctx->in中的链表项追加到out中
				 *
				 */
                last_out = &cl->next;
                cl->next = NULL;

                continue;
            }

            if (ctx->buf == NULL) {

            	/*
            	 * 走到这里说明需要进行数据拷贝了,拷贝之前需要先创建一个临时buf,大小以bsize为参考
            	 *
            	 * bsize是ctx->in->buf中实际使用的缓存大小
            	 *
            	 */
                rc = ngx_output_chain_align_file_buf(ctx, bsize);

                if (rc == NGX_ERROR) {
                    return NGX_ERROR;
                }

                if (rc != NGX_OK) {

                    if (ctx->free) {

                        /* get the free buf */

                    	/*
                    	 * 从ctx->free中获取一个buf
                    	 *
                    	 * ctx->free的作用是重新利用free中的buf,buf对应的chain会被释放到ctx->pool中去
                    	 * 所以当分配buf不成功的时候会试着从ctx->free中获取
                    	 */

                        cl = ctx->free;
                        ctx->buf = cl->buf;
                        ctx->free = cl->next;

                        /*
                         * 释放掉和cl->buf关联的cl
                         */
                        ngx_free_chain(ctx->pool, cl);

                    } else if (out || ctx->allocated == ctx->bufs.num) {

                    	/*
                    	 * 如果实际已经创建的buf达到了我们配置的个数(ctx->bufs.num),就不再创建新的buf而是等数据
                    	 * 输出完毕后使用原来分配的buf(ctx->free)
                    	 */
                        break;

                        /*
                         * 创建一个新的buf并分配内存
                         */
                    } else if (ngx_output_chain_get_buf(ctx, bsize) != NGX_OK) {
                        return NGX_ERROR;
                    }
                }
            }

            /*
             * 走到这里说明ctx->in->buf中的数据不能直接发送出去,需要先拷贝到ctx->buf中,然后在把ctx->buf追加到out链表中
             *
             * 如果ctx->in->buf中的数据在内存中则直接拷贝到ctx->buf中
             *
             * 如果ctx->in->buf中的数据在文件中,并且开启redirectio,则文件中的数据会被拷贝到ctx->buf中
             *
             * 如果ctx->in->buf中的数据在文件中,并且支持sendfile,则不发生实际拷贝,只是把buf中的文件偏移量赋值给ctx->buf
             * 	(貌似这种情况会被上面的if (ngx_output_chain_as_is(ctx, ctx->in->buf))逻辑给截胡)
             *
             */
            rc = ngx_output_chain_copy_buf(ctx);

            if (rc == NGX_ERROR) {
                return rc;
            }

            if (rc == NGX_AGAIN) {
                if (out) {
                    break;
                }

                return rc;
            }

            /* delete the completed buf from the ctx->in chain */

            if (ngx_buf_size(ctx->in->buf) == 0) {

            	/*
            	 * 如果ctx->in->buf完全处理完毕后就让ctx->in链表指向下一个链表项
            	 *
            	 * 举一个buf不会一次处理完毕的情况:
            	 *   假设buf中的数据在文件中,并且数据非常大,并且超过了ctx->bufs中指定的大小,那么一个ctx->in->buf
            	 *   就会被拆分成多个buf而被追加到out链表中
            	 */
                ctx->in = ctx->in->next;
            }

            /*
             * 为上面拷贝好的ctx->buf分配一个ngx_chain_t进行关联
             *
             */
            cl = ngx_alloc_chain_link(ctx->pool);
            if (cl == NULL) {
                return NGX_ERROR;
            }

            // 拷贝好的ctx->buf赋值给cl->buf
            cl->buf = ctx->buf;
            cl->next = NULL;
            *last_out = cl;  // 追加到out链表尾部
            last_out = &cl->next;  // last_out指向就是out链表尾部链表项的next变量的地址
            ctx->buf = NULL; // 清空临时buf,供下次拷贝是使用
        }

        if (out == NULL && last != NGX_NONE) {

            if (ctx->in) {
                return NGX_AGAIN;
            }

            return last;
        }

        /*
         * 调用下一个过滤器,将out中的数据输出,如果out中的数据没有一次输出完毕会放到ctx->busy中继续输出,之后
         * 只有不阻塞就会多次调用下一个过滤器进行输出
         *
         * 注意这里的out不一定是全部的数据,比如不支持sendfile,那么文件中的数据不一定可以被完全读完
         */
        last = ctx->output_filter(ctx->filter_ctx, out);

        if (last == NGX_ERROR || last == NGX_DONE) {
            return last;
        }

        /*
         * 将ctx->busy和out链表中空闲的链表项释放到ctx->free链表中,前提是这这两个链表中缓存的tag(ctx->busy->buf->tag或out->buf->tag)
         * 和ctx->tag(&ngx_http_copy_filter_module)相同,否则直接释放到ctx->pool->chain链表中
         */
        ngx_chain_update_chains(ctx->pool, &ctx->free, &ctx->busy, &out,
                                ctx->tag);

        /*
         * ngx_chain_update_chains()方法执行完毕后out变量一定为NULL
         *
         * 把out变量的地址赋值last_out,在下次循环中继续组装链表
         */
        last_out = &out;
    }
}


/*
 * linux中的aio需要开启directio
 *
 * 返回1表示可以把数据直接发送出去,不需要拷贝该buf数据:
 * 	  如果buf中的数据在内存中则使用ngx_writev()方法发送
 * 	  如果buf中的数据在文件中(buf->in_file==1)则使用ngx_linux_sendfile()方法发送出去
 *
 * 返回0则表示数据还在磁盘文件内,需要把磁盘中的数据读取到内存才能发出去:
 * 	  如果file有directio标记则并且开启了aio则使用aio读文件
 * 	  如果没有则就只能用传统的方式把数据读到内存(比如pread方法)
 *
 */
static ngx_inline ngx_int_t
ngx_output_chain_as_is(ngx_output_chain_ctx_t *ctx, ngx_buf_t *buf)
{
    ngx_uint_t  sendfile;

    if (ngx_buf_special(buf)) {
        return 1;
    }

#if (NGX_THREADS)
    if (buf->in_file) {
        buf->file->thread_handler = ctx->thread_handler;
        buf->file->thread_ctx = ctx->filter_ctx;
    }
#endif

    /*
     * 数据在文件中,并且对文件开启了directio(直接io,不走page cache),那就说明后续需要把文件
     * 中的数据读取到内存,然后才能发送,对于linux这个时候一般需要把aio也开启,否则在读数据的时候
     * 就会阻塞了(TODO ?)
     *
     */
    if (buf->in_file && buf->file->directio) {
    	/*
    	 * buf数据在文件中,并且开启了directio,则需要把文件数据读到内存才能发出去
    	 */
        return 0;
    }

    sendfile = ctx->sendfile;

#if (NGX_SENDFILE_LIMIT)

    if (buf->in_file && buf->file_pos >= NGX_SENDFILE_LIMIT) {
        sendfile = 0;
    }

#endif

    if (!sendfile) {
    	/*
		 * 当前不支持sendfile系统调用,或者没有开启sendfile指令
		 */

    	/*
    	 * #define ngx_buf_in_memory(b)        (b->temporary || b->memory || b->mmap)
    	 * 检查是否在内存中
    	 */
        if (!ngx_buf_in_memory(buf)) {

        	/*
        	 * 如果当前请求不支持sendfile系统调用,或者没有开启sendfile指令,
        	 * 并且当前buf缓存的数据不在内存中(那就是buf->in_file==1),并且也没有开启buf->file->directio
        	 * 那么说明这个buf不能使用sendfile直接发送出去,也不能使用aio的方式将文件读到内存,需要用传统的方式读文件到内存,然后再发
        	 */
            return 0;
        }

        buf->in_file = 0;
    }

#if (NGX_HAVE_AIO_SENDFILE)
    if (ctx->aio_preload && buf->in_file) {
        (void) ngx_output_chain_aio_setup(ctx, buf->file);
    }
#endif

    if (ctx->need_in_memory && !ngx_buf_in_memory(buf)) {
        return 0;
    }

    if (ctx->need_in_temp && (buf->memory || buf->mmap)) {
        return 0;
    }

    return 1;
}


#if (NGX_HAVE_AIO_SENDFILE)

static ngx_int_t
ngx_output_chain_aio_setup(ngx_output_chain_ctx_t *ctx, ngx_file_t *file)
{
    ngx_event_aio_t  *aio;

    if (file->aio == NULL && ngx_file_aio_init(file, ctx->pool) != NGX_OK) {
        return NGX_ERROR;
    }

    aio = file->aio;

    aio->data = ctx->filter_ctx;
    aio->preload_handler = ctx->aio_preload;

    return NGX_OK;
}

#endif


/*
 * 把链表in中的buf,追加到*chain链中
 * 这里并没有发生实际的buf拷贝动作,只是追加的过程中抛弃了in中的ngx_chian_t,重新获取新的
 * ngx_chian_t来关联in中的buf
 */
static ngx_int_t
ngx_output_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain,
    ngx_chain_t *in)
{
    ngx_chain_t  *cl, **ll;
#if (NGX_SENDFILE_LIMIT)
    ngx_buf_t    *b, *buf;
#endif

    ll = chain;

    for (cl = *chain; cl; cl = cl->next) {
        ll = &cl->next;
    }

    while (in) {

        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

#if (NGX_SENDFILE_LIMIT)

        buf = in->buf;

        if (buf->in_file
            && buf->file_pos < NGX_SENDFILE_LIMIT
            && buf->file_last > NGX_SENDFILE_LIMIT)
        {
            /* split a file buf on two bufs by the sendfile limit */

            b = ngx_calloc_buf(pool);
            if (b == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(b, buf, sizeof(ngx_buf_t));

            if (ngx_buf_in_memory(buf)) {
                buf->pos += (ssize_t) (NGX_SENDFILE_LIMIT - buf->file_pos);
                b->last = buf->pos;
            }

            buf->file_pos = NGX_SENDFILE_LIMIT;
            b->file_last = NGX_SENDFILE_LIMIT;

            cl->buf = b;

        } else {
            cl->buf = buf;
            in = in->next;
        }

#else
        cl->buf = in->buf;
        in = in->next;

#endif

        cl->next = NULL;
        *ll = cl;
        ll = &cl->next;
    }

    return NGX_OK;
}


/*
 * 获取一个对齐文件的buf
 *
 */
static ngx_int_t
ngx_output_chain_align_file_buf(ngx_output_chain_ctx_t *ctx, off_t bsize)
{
    size_t      size;
    ngx_buf_t  *in;

    in = ctx->in->buf;

    if (in->file == NULL || !in->file->directio) {

    	/*
		 * 因为一般情况我们很少开启directio所以大部分情况会走这段逻辑
		 */
        return NGX_DECLINED;
    }

    /*
     * 走到这里说明数据在文件中,并且开启了directio
     *
     * 所以这里对ctx->directio类型的内存分配和,ngx_output_chain_get_buf()方法中对ctx->directio类型
     * 的内存分配的对齐方式是不一样的
     * 	  一个使用ngx_create_temp_buf(pool, size)方法分配
     * 	  一个使用ngx_pmemalign(pool,size,ctx->alignment)方法分配
     * 为啥要不一样? TODO 这算什么形式的优化?
     *
     */

    ctx->directio = 1;

    /*
     * 这一句是啥意思,等于零又是啥意思
     */
    size = (size_t) (in->file_pos - (in->file_pos & ~(ctx->alignment - 1)));

    if (size == 0) {

    	/*
    	 * 如果要分配的内存大于等于设置的(ctx->bufs.size)内存,那就直接返回,后续的逻辑会尝试从ctx->free中
    	 * 或者使用ngx_output_chain_get_buf()方法来创建这块缓存
    	 */

        if (bsize >= (off_t) ctx->bufs.size) {
            return NGX_DECLINED;
        }

        size = (size_t) bsize;

    } else {
        size = (size_t) ctx->alignment - size;

        if ((off_t) size > bsize) {
            size = (size_t) bsize;
        }
    }

    /*
     * 在这里创建的buf会突破ctx->bufs.num的限制
     * 并且因为该buf没有设置对应的tag,所以也不会释放到ctx->free中,而是会被释放到ctx->pool
     */
    ctx->buf = ngx_create_temp_buf(ctx->pool, size);
    if (ctx->buf == NULL) {
        return NGX_ERROR;
    }

    /*
     * we do not set ctx->buf->tag, because we do not want
     * to reuse the buf via ctx->free list
     */

#if (NGX_HAVE_ALIGNED_DIRECTIO)
    ctx->unaligned = 1;
#endif

    return NGX_OK;
}


/*
 * 创建一个ngx_buf_t并为其分配内存,以bsize为参考来分配
 * 		ctx->bufs.size <= bsize <= ctx->bufs.size * 1.25
 *
 *
 */
static ngx_int_t
ngx_output_chain_get_buf(ngx_output_chain_ctx_t *ctx, off_t bsize)
{
    size_t       size;
    ngx_buf_t   *b, *in;
    ngx_uint_t   recycled;

    in = ctx->in->buf;
    size = ctx->bufs.size;
    recycled = 1;

    if (in->last_in_chain) {

        if (bsize < (off_t) size) {
        	/*
			 * 如果缓存in在当前链表中位于最后一个链表项,并且要分配的内存比配置的ctx->bufs.size要小,并且未开启
			 * directio,那么就是用bsize来为ctx->buf分配内存大小
			 *
			 */

            /*
             * allocate a small temp buf for a small last buf
             * or its small last part
             */

            size = (size_t) bsize;
            recycled = 0;

        } else if (!ctx->directio
                   && ctx->bufs.num == 1
                   && (bsize < (off_t) (size + size / 4)))
        {
        	/*
        	 * 如果缓存in在当前链表中位于最后一个链表项,并且未开启directio,并且我们配置最多使用一个buf来处理数据,并且
        	 * 要分配的内存我们设置的buf的大小的1.25倍,那么我们就直接用bsize来分配buf大小,也就是说buf大小可以超25%
        	 *
        	 */

            /*
             * allocate a temp buf that equals to a last buf,
             * if there is no directio, the last buf size is lesser
             * than 1.25 of bufs.size and the temp buf is single
             */

            size = (size_t) bsize;
            recycled = 0;
        }
    }

    // 创建一个ngx_buf_t结构体
    b = ngx_calloc_buf(ctx->pool);
    if (b == NULL) {
        return NGX_ERROR;
    }

    if (ctx->directio) {

        /*
         * allocate block aligned to a disk sector size to enable
         * userland buffer direct usage conjunctly with directio
         */

    	/*
    	 * 如果开启directio,那么ngx会直接跟磁盘打交道不走page cache,所以这里就要自己考虑内存地址和磁盘
    	 * 块对齐的行为
    	 */

        b->start = ngx_pmemalign(ctx->pool, size, (size_t) ctx->alignment);
        if (b->start == NULL) {
            return NGX_ERROR;
        }

    } else {

    	/*
    	 * 如果没有开启directio则表明ngx不会绕过内存直接跟磁盘打交道,直接使用ngx默认对齐方式
    	 */

        b->start = ngx_palloc(ctx->pool, size);
        if (b->start == NULL) {
            return NGX_ERROR;
        }
    }

    b->pos = b->start;
    b->last = b->start;
    b->end = b->last + size;
    b->temporary = 1; // 临时内存
    b->tag = ctx->tag; // 标记这个buf
    b->recycled = recycled;

    ctx->buf = b;
    ctx->allocated++; // 实际分配的buf加一

    return NGX_OK;
}


/*
 * ctx->in->buf中的数据都拷贝到ctx->buf中
 *
 * 如果ctx->in->buf中的数据在内存中则直接拷贝到ctx->buf中
 *
 * 如果ctx->in->buf中的数据在文件中,并且开启redirectio,则文件中的数据会被拷贝到ctx->buf中
 *
 * 如果ctx->in->buf中的数据在文件中,并且开始sendfile,则不发生实际拷贝,只是把buf中的文件偏移量赋值给ctx->buf
 *
 */
static ngx_int_t
ngx_output_chain_copy_buf(ngx_output_chain_ctx_t *ctx)
{
    off_t        size;
    ssize_t      n;
    ngx_buf_t   *src, *dst;
    ngx_uint_t   sendfile;

    /*
     * 将源src中的数据拷贝到目标dst中
     */
    src = ctx->in->buf;
    dst = ctx->buf;

    size = ngx_buf_size(src);
    size = ngx_min(size, dst->end - dst->pos);

    sendfile = ctx->sendfile & !ctx->directio;

#if (NGX_SENDFILE_LIMIT)

    if (src->in_file && src->file_pos >= NGX_SENDFILE_LIMIT) {
        sendfile = 0;
    }

#endif

    if (ngx_buf_in_memory(src)) {
        ngx_memcpy(dst->pos, src->pos, (size_t) size);
        src->pos += (size_t) size;
        dst->last += (size_t) size;

        if (src->in_file) {

            if (sendfile) {
            	/*
            	 * 如果当前buf(src)数据在文件中,并且ngx支持sendfile方法,那么就把文件的数据偏移量赋值给新的buf(dst)
            	 *
            	 */

                dst->in_file = 1;
                dst->file = src->file;
                dst->file_pos = src->file_pos;
                dst->file_last = src->file_pos + size;

            } else {
            	/*
            	 * 如果当前buf(src)数据在文件中,但是ngx不支持sendfile方法,那么就把新buf(dst)中的in_file标志设置为0,
            	 * 并在后面是用ngx_read_file()或ngx_file_aio_read()方法将数据读到新buf(dst)中
            	 *
            	 */

                dst->in_file = 0;
            }

            src->file_pos += size;

        } else {
            dst->in_file = 0;
        }

        if (src->pos == src->last) {
            dst->flush = src->flush;
            dst->last_buf = src->last_buf;
            dst->last_in_chain = src->last_in_chain;
        }

    } else {

#if (NGX_HAVE_ALIGNED_DIRECTIO)

        if (ctx->unaligned) {
            if (ngx_directio_off(src->file->fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, ngx_errno,
                              ngx_directio_off_n " \"%s\" failed",
                              src->file->name.data);
            }
        }

#endif

#if (NGX_HAVE_FILE_AIO)
        if (ctx->aio_handler) {

        	/*
        	 * 如果支持并开启aio会走该逻辑,但是如果没有打开redirectio,那么最终还是会用ngx_read_file()方法去读数据
        	 */
            n = ngx_file_aio_read(src->file, dst->pos, (size_t) size,
                                  src->file_pos, ctx->pool);
            if (n == NGX_AGAIN) {
                ctx->aio_handler(ctx, src->file);
                return NGX_AGAIN;
            }

        } else
#endif
#if (NGX_THREADS)
        if (src->file->thread_handler) {
            n = ngx_thread_read(&ctx->thread_task, src->file, dst->pos,
                                (size_t) size, src->file_pos, ctx->pool);
            if (n == NGX_AGAIN) {
                ctx->aio = 1;
                return NGX_AGAIN;
            }

        } else
#endif
        {
        	/*
        	 * 将源文件src中的数据拷贝到目标dst中
        	 *
        	 * 如果开启redirectio但没有开启aio,则使用这个方法读文件数据
        	 *
        	 * 如果开启了aio但是没有开启redirectio,最终还是会使用ngx_read_file()方法去读文件数据
        	 * 只不过触发ngx_read_file()方法的调用用的是前面的ngx_file_aio_read()方法
        	 *
        	 */
            n = ngx_read_file(src->file, dst->pos, (size_t) size,
                              src->file_pos);
        }

#if (NGX_HAVE_ALIGNED_DIRECTIO)

        if (ctx->unaligned) {
            ngx_err_t  err;

            err = ngx_errno;

            if (ngx_directio_on(src->file->fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, ngx_errno,
                              ngx_directio_on_n " \"%s\" failed",
                              src->file->name.data);
            }

            ngx_set_errno(err);

            ctx->unaligned = 0;
        }

#endif

        if (n == NGX_ERROR) {
            return (ngx_int_t) n;
        }

        if (n != size) {
            ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                          ngx_read_file_n " read only %z of %O from \"%s\"",
                          n, size, src->file->name.data);
            return NGX_ERROR;
        }

        dst->last += n;

        if (sendfile) {
            dst->in_file = 1;
            dst->file = src->file;
            dst->file_pos = src->file_pos;
            dst->file_last = src->file_pos + n;

        } else {
            dst->in_file = 0;
        }

        src->file_pos += n;

        if (src->file_pos == src->file_last) {
            dst->flush = src->flush;
            dst->last_buf = src->last_buf;
            dst->last_in_chain = src->last_in_chain;
        }
    }

    return NGX_OK;
}


ngx_int_t
ngx_chain_writer(void *data, ngx_chain_t *in)
{
    ngx_chain_writer_ctx_t *ctx = data;

    off_t              size;
    ngx_chain_t       *cl, *ln, *chain;
    ngx_connection_t  *c;

    c = ctx->connection;

    for (size = 0; in; in = in->next) {

#if 1
        if (ngx_buf_size(in->buf) == 0 && !ngx_buf_special(in->buf)) {

            ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                          "zero size buf in chain writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          in->buf->temporary,
                          in->buf->recycled,
                          in->buf->in_file,
                          in->buf->start,
                          in->buf->pos,
                          in->buf->last,
                          in->buf->file,
                          in->buf->file_pos,
                          in->buf->file_last);

            ngx_debug_point();

            continue;
        }
#endif

        size += ngx_buf_size(in->buf);

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, c->log, 0,
                       "chain writer buf fl:%d s:%uO",
                       in->buf->flush, ngx_buf_size(in->buf));

        cl = ngx_alloc_chain_link(ctx->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = in->buf;
        cl->next = NULL;
        *ctx->last = cl;
        ctx->last = &cl->next;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "chain writer in: %p", ctx->out);

    for (cl = ctx->out; cl; cl = cl->next) {

#if 1
        if (ngx_buf_size(cl->buf) == 0 && !ngx_buf_special(cl->buf)) {

            ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                          "zero size buf in chain writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();

            continue;
        }
#endif

        size += ngx_buf_size(cl->buf);
    }

    if (size == 0 && !c->buffered) {
        return NGX_OK;
    }

    chain = c->send_chain(c, ctx->out, ctx->limit);

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "chain writer out: %p", chain);

    if (chain == NGX_CHAIN_ERROR) {
        return NGX_ERROR;
    }

    for (cl = ctx->out; cl && cl != chain; /* void */) {
        ln = cl;
        cl = cl->next;
        ngx_free_chain(ctx->pool, ln);
    }

    ctx->out = chain;

    if (ctx->out == NULL) {
        ctx->last = &ctx->out;

        if (!c->buffered) {
            return NGX_OK;
        }
    }

    return NGX_AGAIN;
}
