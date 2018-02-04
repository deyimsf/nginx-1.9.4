
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


/**
 * 从hash表中查找一个元素
 *
 * *hash: 散列表
 * key: 键值对中键的hash值
 * *name: 键值对中键的名字
 * len: 键名字的长度
 */
void *
ngx_hash_find(ngx_hash_t *hash, ngx_uint_t key, u_char *name, size_t len)
{
    ngx_uint_t       i;
    ngx_hash_elt_t  *elt;

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "hf:\"%*s\"", len, name);
#endif

    // 计算所在桶的起始地址
    elt = hash->buckets[key % hash->size];

    if (elt == NULL) {
        return NULL;
    }

    while (elt->value) {
    	// 比较键的长度是否相同
        if (len != (size_t) elt->len) {
            goto next;
        }

        // 比较键的名字是否相同
        for (i = 0; i < len; i++) {
            if (name[i] != elt->name[i]) {
                goto next;
            }
        }

        // 返回找到的元素
        return elt->value;

    next:

		// 当前元素键的名字的首地址 加上 当前元素键的名字的长度,按4字节对齐内存,
		// 之后得出的结果就是下一个元素的首地址
        elt = (ngx_hash_elt_t *) ngx_align_ptr(&elt->name[0] + elt->len,
                                               sizeof(void *));
        continue;
    }

    return NULL;
}


/**
 * 从前置通配符散列表中查找元素
 *
 * *hwc: 前置通配符散列表
 * *name: key的名字
 * len: key名字的长度
 */
void *
ngx_hash_find_wc_head(ngx_hash_wildcard_t *hwc, u_char *name, size_t len)
{
    void        *value;
    ngx_uint_t   i, n, key;

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "wch:\"%*s\"", len, name);
#endif

    n = len;

    // www.jd.com
    while (n) {
        if (name[n - 1] == '.') {
            break;
        }

        n--;
    }

    key = 0;

    for (i = n; i < len; i++) {
        key = ngx_hash(key, name[i]);
    }

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "key:\"%ui\"", key);
#endif

    value = ngx_hash_find(&hwc->hash, key, &name[n], len - n);

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "value:\"%p\"", value);
#endif

    if (value) {

        /*
         * the 2 low bits of value have the special meaning:
         *     00 - value is data pointer for both "example.com"
         *          and "*.example.com";
         *     01 - value is data pointer for "*.example.com" only;
         *     10 - value is pointer to wildcard hash allowing
         *          both "example.com" and "*.example.com";
         *     11 - value is pointer to wildcard hash allowing
         *          "*.example.com" only.
         */

        if ((uintptr_t) value & 2) {

            if (n == 0) {

                /* "example.com" */

                if ((uintptr_t) value & 1) {
                    return NULL;
                }

                hwc = (ngx_hash_wildcard_t *)
                                          ((uintptr_t) value & (uintptr_t) ~3);
                return hwc->value;
            }

            hwc = (ngx_hash_wildcard_t *) ((uintptr_t) value & (uintptr_t) ~3);

            value = ngx_hash_find_wc_head(hwc, name, n - 1);

            if (value) {
                return value;
            }

            return hwc->value;
        }

        if ((uintptr_t) value & 1) {

            if (n == 0) {

                /* "example.com" */

                return NULL;
            }

            return (void *) ((uintptr_t) value & (uintptr_t) ~3);
        }

        return value;
    }

    return hwc->value;
}


void *
ngx_hash_find_wc_tail(ngx_hash_wildcard_t *hwc, u_char *name, size_t len)
{
    void        *value;
    ngx_uint_t   i, key;

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "wct:\"%*s\"", len, name);
#endif

    key = 0;

    // www.jd.com   www.jd.
    for (i = 0; i < len; i++) {
        if (name[i] == '.') {
            break;
        }

        key = ngx_hash(key, name[i]);
    }

    if (i == len) {
        return NULL;
    }

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "key:\"%ui\"", key);
#endif

    // name = www.
    value = ngx_hash_find(&hwc->hash, key, name, i);

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "value:\"%p\"", value);
#endif

    if (value) {

        /*
         * the 2 low bits of value have the special meaning:
         *     00 - value is data pointer;
         *     11 - value is pointer to wildcard hash allowing "example.*".
         */

        if ((uintptr_t) value & 2) {

            i++;

            hwc = (ngx_hash_wildcard_t *) ((uintptr_t) value & (uintptr_t) ~3);

            value = ngx_hash_find_wc_tail(hwc, &name[i], len - i);

            if (value) {
                return value;
            }

            return hwc->value;
        }

        return value;
    }

    return hwc->value;
}


/**
 * 从联合散列表中查找元素
 *
 * 联合散列表ngx_hash_combined_t包含三个基本散列表:精确匹配的散列表、匹配前置通配符的散列表、匹配后置通配符的散列表
 *
 * *hash: 散列表
 * key: 键值对中键的hash值
 * *name: 键值对中键的名字
 * len: 键名字的长度
 */
void *
ngx_hash_find_combined(ngx_hash_combined_t *hash, ngx_uint_t key, u_char *name,
    size_t len)
{
    void  *value;

    if (hash->hash.buckets) {
        value = ngx_hash_find(&hash->hash, key, name, len);

        if (value) {
            return value;
        }
    }

    if (len == 0) {
        return NULL;
    }

    if (hash->wc_head && hash->wc_head->hash.buckets) {
        value = ngx_hash_find_wc_head(hash->wc_head, name, len);

        if (value) {
            return value;
        }
    }

    if (hash->wc_tail && hash->wc_tail->hash.buckets) {
        value = ngx_hash_find_wc_tail(hash->wc_tail, name, len);

        if (value) {
            return value;
        }
    }

    return NULL;
}


/**
 *
 * 计算实际用于存储hash元素的ngx_hash_elt_t结构体所需字节个数
 *
 * typedef struct {
 *   void             *value;
 *   u_short           len;
 *   u_char            name[1];
 * } ngx_hash_elt_t;
 *
 * name: ngx_hash_key_t结构指针
 * 		入参是ngx_hash_key_t结构体而不是ngx_hash_elt_t,是因为刚开始的时候,数据信息都存放
 * 		在了ngx_hash_key_t结构体中
 *
 * 计算方式如下：
 * 1. sizeof(void *): 代表结构体ngx_hash_elt_t中的第一个字段 void *value;
 * 2. (name)->key.len: 代表键值对中的键的长度,也就是ngx_hash_key_t中的key的长度(实际上就是ngx_hash_elt_t结构体中的len的值)
 * 3. 2: 代表结构体ngx_hash_elt_t中字段 u_short len 本身占用的字节个数,实际上写成 sizeof(u_short)会更好
 * 4. 因为len这个值的本身已经包含了结构体ngx_hash_elt_t中的name[1],所以不需要加上这个字段的值了
 *
 * 如果不考虑内存对齐,正确的计算结果应该是上面的三个相加。
 * 一个结构体的对齐大小,是按照结构体中最大的那个字段类型的大小来对齐的,所以在ngx_hash_elt_t结构体中,占用字节最多的类型
 * 显然是 (void *) 这个指针,那么它本身显然满足对其要求。字段len的类型u_short的大小是2,显然不满足,另外key的长度只有在
 * 运行时才能知道,它是否满足对齐是个未知数,所以需要将两个不满足的值加起来,并按照 (void *)类型大小对齐就可以了。
 *
 */
#define NGX_HASH_ELT_SIZE(name)                                               \
    (sizeof(void *) + ngx_align((name)->key.len + 2, sizeof(void *)))

/**
 * 初始化散列表,创建散列表并将names的数据赋值给散列表,最后实际的数据并没有发生拷贝
 * names中的对应的hash值有使用方提供,该方法中不会默认hash值
 *
 * 这个方法并没考虑names数组中的重复键(key),后添加的键值对会覆盖前面添加的,所以在使用这个方法时,应该有使用方来控制names中的
 * 重复值,比如ngx_hash_add_key()方法
 *
 * *hinit: 用于构造hash结构的临时结构体
 * *names: hash机构中要存入的键值对,该入参是个键值对数组
 * nelts: 键值对个数,也就是names的个数
 */
ngx_int_t
ngx_hash_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names, ngx_uint_t nelts)
{
    u_char          *elts;
    size_t           len;
    u_short         *test;
    ngx_uint_t       i, n, key, size, start, bucket_size;
    ngx_hash_elt_t  *elt, **buckets;

    /*
     * 如果指定的桶数为0则直接返回错误,桶个数为0则根本无法保存数据
     */
    if (hinit->max_size == 0) {
        ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                      "could not build %s, you should "
                      "increase %s_max_size: %i",
                      hinit->name, hinit->name, hinit->max_size);
        return NGX_ERROR;
    }

    /*
     * 遍历所有要放入散列表的元素大小,对于给定的桶的大小(bucket_size),如果小于某一个要加入的元素大小,
     * 那么说明分配的通的容量无法满足元素的存储,则返回错误(NGX_ERROR)
     */
    for (n = 0; n < nelts; n++) {
        if (hinit->bucket_size < NGX_HASH_ELT_SIZE(&names[n]) + sizeof(void *))
        {
            ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                          "could not build %s, you should "
                          "increase %s_bucket_size: %i",
                          hinit->name, hinit->name, hinit->bucket_size);
            return NGX_ERROR;
        }
    }

    /*
     * 所以每个桶里面最多可以放u_short表示的最大数字
     *       test
     *       -----
     *       | * |
     *       -----
     *         \
     *         ------------------------------
     *         | u_short | u_short | 共hinit->max_size个
     *         -------------------------------
     * 这里的test是为了临时计算出对于给定的数据(names)每个桶应该容纳的字节数量(肯定是要放入的元素的倍数)
     * 以便在后续分配合适的内存
     */
    test = ngx_alloc(hinit->max_size * sizeof(u_short), hinit->pool->log);
    if (test == NULL) {
        return NGX_ERROR;
    }

    /*
     * 减去一个指针的大小是为了预留一个判断位?
     *
     * 根据后面的逻辑可以看出最后每个桶的大小肯定小于等于bucket_size值
     */
    bucket_size = hinit->bucket_size - sizeof(void *);

    start = nelts / (bucket_size / (2 * sizeof(void *)));//好麻烦的算法
    start = start ? start : 1;

    // 如果指定的桶的个数大于1万，并且指定的桶的个数除以实际元素个数小于100，
    // 则实际桶的个数要减去1000
    if (hinit->max_size > 10000 && nelts && hinit->max_size / nelts < 100) {
        start = hinit->max_size - 1000;
    }

    /*
     * start在这里表示实际桶的个数,是一个尝试值
     * 最终size值必须不大于hinit->max_size,否则就使用我们指定的桶个数
     *
     * 下面这个循环目的是,根据实际的要放入的元素的个数,计算出合适的桶个数,以及每个桶的合适大小
     * 计算的合适的桶的个数放在size中
     * 每个桶的大小放在test数组中
     *           test
     *           -----
     *           | * |
     *           -----
     *             \
     *             ---------------
     *             | 20 | 28 |  总共size个
     *             --------------
     * 其中4代表第一个桶大小是20个字节,28代表第二个桶大小是28个字节
     *
     * 从size等于start开始尝试计算
     */
    for (size = start; size <= hinit->max_size; size++) {

    	/*
    	 * 初始化数组test前size个元素值为零
    	 *
    	 * 前面用ngx_alloc()方法为test分配了hinit->max_size个sizeof(u_short)大小的空间,但是这里只初始化
    	 * 了size个,因为很有可能根本就不需要max_size个u_short大小的空间,所以这里能省就省了
    	 */
        ngx_memzero(test, size * sizeof(u_short));

        /*
         * 开始遍历所有的键值对数据
         */
        for (n = 0; n < nelts; n++) {
            if (names[n].key.data == NULL) {
            	/*
            	 * key不能为空
            	 */
                continue;
            }

            /*
             * 计算该元素桶的所在下标
             */
            key = names[n].key_hash % size;

            /*
             * 将该元素所占的字节个数累加到所对应的桶中
             *
             * 之前元素的大小加上该元素的大小,这个元素的大小不包括元素键值对中的value值的大小
             */
            test[key] = (u_short) (test[key] + NGX_HASH_ELT_SIZE(&names[n]));

#if 0
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: %ui %ui \"%V\"",
                          size, key, test[key], &names[n].key);
#endif

            /*
             * 对于当前尝试的桶个数size,如果有一个桶的大小超过了我们设置的桶的大小bucket_size(指定的值减去一个指针大小),
             * 那就说明当前尝试的size不合适,需要把size放大然后再次尝试
             */
            if (test[key] > (u_short) bucket_size) {
                goto next;
            }
        }

        /*
         * 计算出了实际需要分配的桶的个数(小于等于指定的桶的数量)
         * 比如实际的计算结果如下:
         * 			test
         * 			-----
         * 			| * |
         * 			-----
         * 			  \
         * 			  ---------------------
         * 			  | 20 | 40 | 40 | 30 |
         * 			  ---------------------
         * 从上图可以看出size是4,每个桶的大小也不一样,总共需要130个字节来存储数据
         *
         */
        goto found;

    next:

        continue;
    }

    /*
     * 走到这里说明用户指定的桶的个数也不够大(不够优)
     * 按照ngx的意思,每个桶里面放的元素个数太多了,所以它在这里打出一个警告信息
     */
    size = hinit->max_size;

    ngx_log_error(NGX_LOG_WARN, hinit->pool->log, 0,
                  "could not build optimal %s, you should increase "
                  "either %s_max_size: %i or %s_bucket_size: %i; "
                  "ignoring %s_bucket_size",
                  hinit->name, hinit->name, hinit->max_size,
                  hinit->name, hinit->bucket_size, hinit->name);

found:

	/* size为计算出的实际的桶的个数 */
    for (i = 0; i < size; i++) {
    	/*
    	 * 全部初始化为一个指针大小,对于64为系统来说就是8
    	 *
    	 * 因为前面在计算桶大小的时候减掉一个指针大小,所以这里再加上来
    	 *		bucket_size = hinit->bucket_size - sizeof(void *);
    	 */
        test[i] = sizeof(void *);
    }

    /*
     * 下面这个循环的目的是把之前算好的test的每个桶的大小都再加上一个指针大小
     *
     * 一个问题:
     * 	 为啥不直接加,而是要重新计算一次啊,cpu计算不要钱吗?
     */
    for (n = 0; n < nelts; n++) {
        if (names[n].key.data == NULL) {
            continue;
        }

        /* 计算数据names[n]所在桶的下标 */
        key = names[n].key_hash % size;
        /*
         * 数据names[n]需要站的字节大小累加到对应的桶中
         */
        test[key] = (u_short) (test[key] + NGX_HASH_ELT_SIZE(&names[n]));
    }

    len = 0;

    /*
     * 这个循环下来就会计算出,按ngx_cacheline_size个字节大小对齐后每个桶的实际大小,所以只有到这里才能正真
     * 计算出每个桶的大小,后续按这个大小来分配内存
     */
    for (i = 0; i < size; i++) {
    	/*
    	 * 对于64系统sizeof(void *)值是8,这里用这个值判断每个桶的结尾,问题是万一有一个桶的大小正好是8,那
    	 * 岂不是会出错?
    	 *
    	 * 其实这里有一个潜规则,我们知道ngx的hash结果存放的是ngx_hash_elt_t结构体对象,由这个结构体的定义:
    	 * 	typedef struct {
		 *		void             *value;
		 *		u_short           len;
		 *		u_char            name[1];
		 *	} ngx_hash_elt_t;
		 * 和NGX_HASH_ELT_SIZE宏的计算方式可以看出,每个桶的大小根本不会小于11个字节,所以这里用上面预留的
		 * 一个指针大小的判断位就可以判断每个桶的结尾
		 *
		 * 所以下面的判断实际上是在过滤掉那些空桶,比如实际上test可能是这样的
		 * 			test
		 * 			-----
		 * 			| * |
		 * 			-----
		 * 			  \
		 * 			  ------------------------
		 * 			  | 8 | 30 | 45 | 8 | 64 |
		 * 			  ------------------------
		 * 数字是8就代表是个空桶
    	 */
        if (test[i] == sizeof(void *)) {
            continue;
        }

        /*
         * 把第i个桶的实际大小按照ngx_cacheline_size对齐,这次计算的值才是桶的真正大小
         *
         * ?会和NGX_HASH_ELT_SIZE(&names[n])计算的值不一样吗?
         * NGX_HASH_ELT_SIZE是计算单个元素的,而ngx_align()方法是在计算某个桶对齐后的大小,也就说整个桶作为一个整体也要对齐
         */
        test[i] = (u_short) (ngx_align(test[i], ngx_cacheline_size));

        /*
         * 累加各个桶的大小,后面就用这个值来分配内存
         */
        len += test[i];
    }

    if (hinit->hash == NULL) {
    	// 为hash结构分配内存
    	// size为桶的个数
        hinit->hash = ngx_pcalloc(hinit->pool, sizeof(ngx_hash_wildcard_t)
                                             + size * sizeof(ngx_hash_elt_t *));
        if (hinit->hash == NULL) {
            ngx_free(test);
            return NGX_ERROR;
        }

        // 指定所有桶的起始地址
        buckets = (ngx_hash_elt_t **)
                      ((u_char *) hinit->hash + sizeof(ngx_hash_wildcard_t));

    } else {
    	/*
    	 * 分配size个桶
    	 *      buckets
    	 *       -----
    	 *       | * |
    	 *       -----
    	 *         \
    	 *         -------------
    	 *         | * | * | size个桶
    	 *         ---------------
    	 */
        buckets = ngx_pcalloc(hinit->pool, size * sizeof(ngx_hash_elt_t *));
        if (buckets == NULL) {
            ngx_free(test);
            return NGX_ERROR;
        }
    }

    /*
     * 为实际存储元素(ngx_hash_elt_t)的散列表分配内存
     * len为之前计算出的字节个数
     */
    elts = ngx_palloc(hinit->pool, len + ngx_cacheline_size);
    if (elts == NULL) {
        ngx_free(test);
        return NGX_ERROR;
    }

    // 按ngx_cacheline_size对齐,因为在分配内存时加上了ngx_cacheline_size,
    // 所以内存对齐后散列表的实际大小不会小于len
    elts = ngx_align_ptr(elts, ngx_cacheline_size);

    /*
     * 将每个桶的起始地址,赋值给每个桶的指针变量,下面的代码执行完毕后可能是如下的结构:
     * 			buckets
     * 			 -----
     * 			 | * |
     * 			 -----
     * 			   \
     * 			   -----------------------
     * 			   | * | * | * | * | * |     size个桶
     * 			   -----------------------
     *              /       /   /      \
     *             ----------------------------------
     * 	           |       len个字节的elts            |
     *	           ----------------------------------
     * 可以看到第二桶没有指向elts这个内存块,所以他是空桶,其它不是空桶
     */
    for (i = 0; i < size; i++) {
        if (test[i] == sizeof(void *)) {
            continue;
        }

        /* 为第i个桶指针变量赋值起始地址 */
        buckets[i] = (ngx_hash_elt_t *) elts;
        elts += test[i];

    }

    /*
     * 重置test,为跟每个桶赋值做准备,因为前面只是为桶分配内存空间
     */
    for (i = 0; i < size; i++) {
        test[i] = 0;
    }

    /*
     * 为桶中的所有元素(ngx_hash_elt_t)赋值
     */
    for (n = 0; n < nelts; n++) {
        if (names[n].key.data == NULL) {
        	/*
        	 * 过滤掉空桶
        	 */
            continue;
        }

        /*
         * 计算第n元素对应的桶所在的下标
         */
        key = names[n].key_hash % size;
        /*
         * 该元素应该存放的起始位置
         */
        elt = (ngx_hash_elt_t *) ((u_char *) buckets[key] + test[key]);

        /*
         * 键值对(key-value)对应的值(value)
         *
         * 这里不知道值具体是什么以及到底多大,只有创造这个hash结构的用户知道
         */
        elt->value = names[n].value;

        /*
         * 键值对(key-value)对应的键(key)的长度
         */
        elt->len = (u_short) names[n].key.len;
        /*
         * 将键值对的键以小写的形式存放到散列表中
         */
        ngx_strlow(elt->name, names[n].key.data, names[n].key.len);

        /*
         * 会和下面的代码计算的值不一样吗?
         * 	  test[i] = (u_short) (ngx_align(test[i], ngx_cacheline_size));
         * ngx_align()方法是在计算整个桶对齐后的大小,而NGX_HASH_ELT_SIZE是计算桶中每个元素的大小
         *
         * 在第key桶中放入该元素后,第key个桶的实际大小,用test临时存储,作用是为下一个元素赋值
         */
        test[key] = (u_short) (test[key] + NGX_HASH_ELT_SIZE(&names[n]));
    }

    /*
     * 前面在计算桶大小和为桶分配内存的时候,为每个桶的最后面预留了一个8字节(64位系统)空间,这里我们把这个值设置为NULL
     * 这样以后再查找的时候,可以根据这个特性做一些逻辑判断
     */
    for (i = 0; i < size; i++) {
        if (buckets[i] == NULL) {
        	/*
        	 * 空桶不做处理
        	 */
            continue;
        }

        /*
         * 之前说每个桶最后预留了一个8字节(64位系统)的空间,但是下面的代码且把这8个字节强转成ngx_hash_elt_t结构体,
         * 原因是这个结构体的一个字段value正好是一个指针变量,在这里我们只操作value这个指针变量,所以正好和预留的8字节
         * 空间对上.
         *
         * 注意,这里是不能操作elt对象其它字段的,因为其它字段是从别处"借来"的
         */
        elt = (ngx_hash_elt_t *) ((u_char *) buckets[i] + test[i]);

        elt->value = NULL;
    }

    /* 至此,test数组就算是用完了 */
    ngx_free(test);

    /*
     * 把桶赋值给下面的hash结构传递出去
     */
    hinit->hash->buckets = buckets;
    /*
     * 桶的实际个数
     */
    hinit->hash->size = size;

#if 0

    for (i = 0; i < size; i++) {
        ngx_str_t   val;
        ngx_uint_t  key;

        elt = buckets[i];

        if (elt == NULL) {
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: NULL", i);
            continue;
        }

        while (elt->value) {
            val.len = elt->len;
            val.data = &elt->name[0];

            key = hinit->key(val.data, val.len);

            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: %p \"%V\" %ui", i, elt, &val, key);

            elt = (ngx_hash_elt_t *) ngx_align_ptr(&elt->name[0] + elt->len,
                                                   sizeof(void *));
        }
    }

#endif

    return NGX_OK;
}


/**
 * 初始化带统配符的散列表
 *
 * *hinit: 用于构造hash结构的临时结构体
 * *names: hash机构中要存入的键值对,该入参是个键值对数组
 *		   全是已经去掉通配符的key,如 com.jd.(*.jd.com)、 com.jd(.jd.com)、 www.jd.(www.jd.*)
 * nelts: 键值对个数,也就是names的个数
 *
 * TODO : 大概意思明白了,但细节没看懂,以后再看吧
 *
 */
ngx_int_t
ngx_hash_wildcard_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts)
{
    size_t                len, dot_len;
    ngx_uint_t            i, n, dot;
    ngx_array_t           curr_names, next_names;
    ngx_hash_key_t       *name, *next_name;
    ngx_hash_init_t       h;
    ngx_hash_wildcard_t  *wdc;

    // 一个临时数组结构,包含nelts个ngx_hash_key_t结构体
    if (ngx_array_init(&curr_names, hinit->temp_pool, nelts,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    // 一个临时数组结构,包含nelts个ngx_hash_key_t结构体
    if (ngx_array_init(&next_names, hinit->temp_pool, nelts,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    //用*names = com. (*.com) 举例
    //用*names = com.jd. (*.jd.com) 举例
    //用*names = com.jd.m. (*.m.jd.com) 举例
    for (n = 0; n < nelts; n = i) {
    	// 这里的n代表第几个元素

#if 0
        ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                      "wc0: \"%V\"", &names[n].key);
#endif

        // 0代表没有发现符号“.”, 1代表发现符号“.”
        dot = 0;

        // 检查第n个元素的键值是否有符号“.”(比如 com. 中的符号“.”)
        // 检查第n个元素的键值是否有符号“.”(比如 com.jd. 中的第一个符号“.”)
        // 检查第n个元素的键值是否有符号“.”(比如 com.jd.m. 中的第一个符号“.”)
        for (len = 0; len < names[n].key.len; len++) {
            if (names[n].key.data[len] == '.') {
                dot = 1;

                // 找到符号“.”之后跳出循环
                // 第一个.  -->  com.
                break;
            }
        }

        // 将字符 com.jd.m. 中的 com 放入curr_names数组中
        name = ngx_array_push(&curr_names);
        if (name == NULL) {
            return NGX_ERROR;
        }

        // len = 3
        name->key.len = len;
        // name->key.data = com
        name->key.data = names[n].key.data;
        name->key_hash = hinit->key(name->key.data, name->key.len);
        // name->value = 1234
        name->value = names[n].value;

        //com.jd.m.
        //name->key.len = 3
        //name->key.data = com
        //name->value = 1234

#if 0
        ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                      "wc1: \"%V\" %ui", &name->key, dot);
#endif

        // 找到的这个“.”在com.jd.中的位置
        dot_len = len + 1;

        if (dot) {
            len++;
        }

        // dot_len = 4     dot = 4
        next_names.nelts = 0;

        // names[n].key.len = 9 (com.jd.m.);     len = 4
        // 相等说明这个域名目前只有一个名字
        if (names[n].key.len != len) {
            next_name = ngx_array_push(&next_names);
            if (next_name == NULL) {
                return NGX_ERROR;
            }

            next_name->key.len = names[n].key.len - len;
            next_name->key.data = names[n].key.data + len;
            next_name->key_hash = 0;
            next_name->value = names[n].value;
            // next_name->key.len = 5
            // next_name->key.data = jd.m.
            // next_name->value = 1234

#if 0
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "wc2: \"%V\"", &next_name->key);
#endif
        }

        // n = 0; i = 1; nelts = 1; 所以不走这段逻辑
        for (i = n + 1; i < nelts; i++) {
            if (ngx_strncmp(names[n].key.data, names[i].key.data, len) != 0) {
                break;
            }

            if (!dot
                && names[i].key.len > len
                && names[i].key.data[len] != '.')
            {
                break;
            }

            next_name = ngx_array_push(&next_names);
            if (next_name == NULL) {
                return NGX_ERROR;
            }

            next_name->key.len = names[i].key.len - dot_len;
            next_name->key.data = names[i].key.data + dot_len;
            next_name->key_hash = 0;
            next_name->value = names[i].value;

#if 0
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "wc3: \"%V\"", &next_name->key);
#endif
        }

        // com.jd.m.  next_names.elts = jd.m.
        if (next_names.nelts) {

            h = *hinit;
            // 这一步非常重要,如果一个域名有多个名字组成就会走这个逻辑
            h.hash = NULL;

            if (ngx_hash_wildcard_init(&h, (ngx_hash_key_t *) next_names.elts,
                                       next_names.nelts)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            wdc = (ngx_hash_wildcard_t *) h.hash;

            if (names[n].key.len == len) {// 感觉永远不会走这段代码呢?
                wdc->value = names[n].value;
            }

            // 有点是11 没点是10(匹配没有 * 的通配符); 比如*.jd.com这种是11   .jd.com这种是10
            name->value = (void *) ((uintptr_t) wdc | (dot ? 3 : 2));

        } else if (dot) {
        	// 将键值对中,值的地址的最后一位设置成1,然后赋值给name->value这个指针变量
        	// 为什么?
            name->value = (void *) ((uintptr_t) name->value | 1);
        }
    }

    if (ngx_hash_init(hinit, (ngx_hash_key_t *) curr_names.elts,
                      curr_names.nelts)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


/**
 * 一个hash方法
 *
 * *data: 要hash的数据的指针
 * len: 要hash的数据的长度
 */
ngx_uint_t
ngx_hash_key(u_char *data, size_t len)
{
    ngx_uint_t  i, key;

    key = 0;

    for (i = 0; i < len; i++) {
        key = ngx_hash(key, data[i]);
    }

    return key;
}


/**
 * 一个hash方法，同ngx_hash_key方法
 * hash之前会将数据转换为小写
 *
 * *data: 要hash的数据的指针
 * len: 要hash的数据的长度
 */
ngx_uint_t
ngx_hash_key_lc(u_char *data, size_t len)
{
    ngx_uint_t  i, key;

    key = 0;

    for (i = 0; i < len; i++) {
        key = ngx_hash(key, ngx_tolower(data[i]));
    }

    return key;
}


ngx_uint_t
ngx_hash_strlow(u_char *dst, u_char *src, size_t n)
{
    ngx_uint_t  key;

    key = 0;

    while (n--) {
        *dst = ngx_tolower(*src);
        key = ngx_hash(key, *dst);
        dst++;
        src++;
    }

    return key;
}


/**
 * 初始化ngx_hash_keys_arrays_t结构体
 *
 * *ha: 要初始化的ngx_hash_keys_arrays_t结构体
 * type: 决定散列表中桶的个数的类型
 *
 */
ngx_int_t
ngx_hash_keys_array_init(ngx_hash_keys_arrays_t *ha, ngx_uint_t type)
{
	// 数组初始元素个数
    ngx_uint_t  asize;

    if (type == NGX_HASH_SMALL) {
        asize = 4;
        ha->hsize = 107;

    } else {
        asize = NGX_HASH_LARGE_ASIZE;
        ha->hsize = NGX_HASH_LARGE_HSIZE;
    }

    // 存放基本散列表键值对的数组,asize为数组初始化大小
    if (ngx_array_init(&ha->keys, ha->temp_pool, asize, sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    // 存放前置通配符散列表键值对的数组,asize为数组初始化大小
    if (ngx_array_init(&ha->dns_wc_head, ha->temp_pool, asize,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    // 存放后置通配符散列表键值对的数组,asize为数组初始化大小
    if (ngx_array_init(&ha->dns_wc_tail, ha->temp_pool, asize,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    // 为基本散列表中的桶分配内存,共ha->hsize个桶
    ha->keys_hash = ngx_pcalloc(ha->temp_pool, sizeof(ngx_array_t) * ha->hsize);
    if (ha->keys_hash == NULL) {
        return NGX_ERROR;
    }

    // 为前置通配符散列表中的桶分配内存,共ha->hsize个桶
    ha->dns_wc_head_hash = ngx_pcalloc(ha->temp_pool,
                                       sizeof(ngx_array_t) * ha->hsize);
    if (ha->dns_wc_head_hash == NULL) {
        return NGX_ERROR;
    }

    // 为后置通配符散列表中的桶分配内存,共ha->hsize个桶
    ha->dns_wc_tail_hash = ngx_pcalloc(ha->temp_pool,
                                       sizeof(ngx_array_t) * ha->hsize);
    if (ha->dns_wc_tail_hash == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


/**
 * 向ngx_hash_keys_arrays_t中添加用户键值对
 *
 * *ha: ngx_hash_keys_arrays_t
 * *key: 用户key,比如“www.jd.com”、“.jd.com”、"*.jd.com"、“www.jd.*”
 * 		 如果key是带通配符的,则在添加的时候会把通配符去掉
 * *value: 用户值,比如“198.168.12.32”或一个结构体
 * flags: 一个位掩码
 * 		NGX_HASH_WILDCARD_KEY: 处理通配符
 * 		NGX_HASH_READONLY_KEY: 不把key转换为小写
 *
 * 不允许存在相同的元素,如果存在返回NGX_BUSY
 *
 * (只为域名key服务,也就是说key不能是其它数据; 也可以是变量;?)
 *
 */
ngx_int_t
ngx_hash_add_key(ngx_hash_keys_arrays_t *ha, ngx_str_t *key, void *value,
    ngx_uint_t flags)
{
    size_t           len;
    u_char          *p;
    ngx_str_t       *name;
    ngx_uint_t       i, k, n, skip, last;
    ngx_array_t     *keys, *hwc;
    ngx_hash_key_t  *hk;

    /* 要添加的key的长度 */
    last = key->len;

    if (flags & NGX_HASH_WILDCARD_KEY) {//支持通配符

        /*
         * supported wildcards:
         *     "*.example.com", ".example.com", and "www.example.*"
         * 以上是ngx中支持的通配符的格式
         *
         */

    	/*
    	 * 是否有通配符,0代表没有
    	 */
        n = 0;

        for (i = 0; i < key->len; i++) {

            if (key->data[i] == '*') {
                if (++n > 1) {
                	/*
                	 * 只支持一个通配符,如果有多个则返回NGX_DECLINED,比如
                	 *    **.jd.com
                	 *    www.jd.**com
                	 *    *w*.jd.com
                	 */
                    return NGX_DECLINED;
                }
            }

            if (key->data[i] == '.' && key->data[i + 1] == '.') {
            	/*
            	 * key不能有连续两个符号“.”,比如
            	 *    www..jd.com
            	 *    ..jd.com
            	 *    www.jd.c..
            	 *
            	 */
                return NGX_DECLINED;
            }
        }

        if (key->len > 1 && key->data[0] == '.') {

        	/*
        	 * 以点开头的域名,如
        	 * 	  .jd.com
        	 * 	  .c
        	 */
            skip = 1;
            goto wildcard;
        }

        if (key->len > 2) {

            if (key->data[0] == '*' && key->data[1] == '.') {

            	/*
            	 * 前置通配符,以 "*." 开头,比如
            	 *   *.jd.com
            	 */
                skip = 2;
                goto wildcard;
            }

            if (key->data[i - 2] == '.' && key->data[i - 1] == '*') {
            	/*
            	 * 后置通配符,以".*"结尾,比如
            	 *     www.jd.*
            	 */
                skip = 0;

                /*
                 * key的长度减去 .* 这两字符的长度
                 */
                last -= 2;
                goto wildcard;
            }
        }

        /*
         * 从上面得出的结论
         * skip = 0,是以".*"结尾,比如
         * 	  www.jd.*
         * skip = 1,以点开头的,比如
         * 	  .jd.com
         * skip = 2,以"*."开头,比如
         *    *.jd.com
         */

        if (n) {
            /*
             * 有通配符,但是又不符合通配符的规则,比如
             * 	  w*w.jd.c*m
             * 	  *.jd.com.*
             */
            return NGX_DECLINED;
        }

        /* 没有通配符 */
    }

    /* exact hash */
    /*************************** 走到这里说明key不带通配符 ******************************/

    /* 此时代表key的hash值 */
    k = 0;

    for (i = 0; i < last; i++) {
        if (!(flags & NGX_HASH_READONLY_KEY)) {
        	// 如果未打标NGX_HASH_READONLY_KEY,则key转换成小写
            key->data[i] = ngx_tolower(key->data[i]);
        }
        k = ngx_hash(k, key->data[i]);
    }

    /*
     * 此时代表key在简易散列表中的第k个桶
     */
    k %= ha->hsize;

    /* check conflicts in exact hash */

    name = ha->keys_hash[k].elts;

    /*
     * name代表桶中的第一个元素,如果第一个元素都不存在那么说明桶为空
     */
    if (name) {

    	/*
    	 * 检查桶中是否有相同的元素,如果有返回NGX_BUSY
    	 */
        for (i = 0; i < ha->keys_hash[k].nelts; i++) {
            if (last != name[i].len) {
            	/*
            	 * 桶中的元素name[i]的长度都和当前要添加的key不一样,那么肯定就不一样,去匹配下一个
            	 */
                continue;
            }

            if (ngx_strncmp(key->data, name[i].data, last) == 0) {

            	/*
            	 * 存在相同元素,则返回
            	 */
                return NGX_BUSY;
            }
        }

    } else {
    	/*
    	 * 用于检查是否有key冲突的桶还不存在,那就创建一个,然后把当前要添加的key放入这个桶中
    	 * 新创建的桶初始化大小为4个字节
    	 */
        if (ngx_array_init(&ha->keys_hash[k], ha->temp_pool, 4,
                           sizeof(ngx_str_t))
            != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    /*
     * 将要添加的key存放到对应的k桶中
     */
    name = ngx_array_push(&ha->keys_hash[k]);
    if (name == NULL) {
        return NGX_ERROR;
    }

    *name = *key;

    /*
     * 确定要添加的key没有冲突之后将key放入到ha->keys数组中,为后续调用ngx_hash_init()生成hash结构做准备
     */
    hk = ngx_array_push(&ha->keys);
    if (hk == NULL) {
        return NGX_ERROR;
    }

    hk->key = *key;
    hk->key_hash = ngx_hash_key(key->data, last);
    hk->value = value;

    /*
     * 以上是处理key不带通配符的代码
     *
     * 作用就是排除相同的key,然后组装ngx_hash_key_t数组
     */
    return NGX_OK;


/*********************************** 处理key中带通配符的逻辑 ************************************/
wildcard:

    /* wildcard hash */

	/*
     * skip = 0,表示以.*结尾的域名,如 www.jd.*
     * skip = 1,表示以点开头,如 .jd.com
     * skip = 2,表示以*.开头,如 *.jd.com
     */

	/*
	 * key转换为小写并计算key的hash值,计算时会去掉通配符,通过skip的值来去掉通配符
	 * skip = 0意思是不用去掉,因为这种类型的通配符已经在前面的逻辑中给去掉了(last -= 2)
	 * skip = 1表示去掉前面一个字符(".")
	 * skip = 2表示去掉前面两个字符("*.")
	 */
    k = ngx_hash_strlow(&key->data[skip], &key->data[skip], last - skip);

    k %= ha->hsize;

    if (skip == 1) {
    	/*
    	 * 处理以点'.'开头的域名
    	 */

        /* check conflicts in exact hash for ".example.com" */

    	/*
    	 * 检查名字是否冲突,比如jd.com和.jd.com视为相同,因为.jd.com已经包含jd.com拉
    	 */
        name = ha->keys_hash[k].elts;

        /*
         * 下面这块逻辑和上面检查无通配符冲突的逻辑基本一样,只不过这里的名字是拷贝过来的,并且这个key没有放到ha->keys数组中
         */
        if (name) { // 桶已经存在
        	// 检查桶中是否有相同的元素

        	// 去掉 .jd.com 中的第一个点的长度
            len = last - skip;

            for (i = 0; i < ha->keys_hash[k].nelts; i++) {
                if (len != name[i].len) {
                    continue;
                }

                if (ngx_strncmp(&key->data[1], name[i].data, len) == 0) {
                	// 存在相同的元素,则直接返回NGX_BUSY
                    return NGX_BUSY;
                }
            }

        } else { // 桶不存在
        	// 初始化桶,初始大小为4
            if (ngx_array_init(&ha->keys_hash[k], ha->temp_pool, 4,
                               sizeof(ngx_str_t))
                != NGX_OK)
            {
                return NGX_ERROR;
            }
        }

        name = ngx_array_push(&ha->keys_hash[k]);
        if (name == NULL) {
            return NGX_ERROR;
        }

        name->len = last - 1;
        name->data = ngx_pnalloc(ha->temp_pool, name->len);
        if (name->data == NULL) {
            return NGX_ERROR;
        }

        /*
         * 将key的名字拷贝过来
         *
         * 但是这里并没有把key放入到ha->keys数组中,因为他不是精确匹配而是通配符匹配
         * 因为.jd.com已经包含jd.com了,所以这两个只留一个就可以了
         */
        ngx_memcpy(name->data, &key->data[1], name->len);
    }

    /*
     * 处理前置通配符
     */
    if (skip) {

        /*
         * convert "*.example.com" to "com.example.\0"
         *      and ".example.com" to "com.example\0"
         */

    	/*
    	 * 此时last包含了通配符的长度
    	 */
        p = ngx_pnalloc(ha->temp_pool, last);
        if (p == NULL) {
            return NGX_ERROR;
        }

        /* 此时len代表一个单词长度 */
        len = 0;
        n = 0;

        /*
         * last-1代表去掉了"*.example.com"中的'*'或者".example.com"中的'.'后的长度
         */
        for (i = last - 1; i; i--) {

            if (key->data[i] == '.') {
                ngx_memcpy(&p[n], &key->data[i + 1], len);
                n += len;
                p[n++] = '.';
                len = 0;
                continue;
            }

            len++;
        }

        /*
         * 如果走到这里len是有值的,那么代表此时key是一个".example.com"类型的字符,因为last-1后会去掉一个通配符
         * 所以我们这里需要把最后一个单词example拷贝到p中,结果是
         * 	  p = com.example
         *
         * 如果走到这里len的值是零,那么代表此时key是一个"*.example.com"类型的字符,那么最后p的结果是
         * 	  p = com.example.
         */
        if (len) {
            ngx_memcpy(&p[n], &key->data[1], len);
            n += len;
        }

        p[n] = '\0';

        hwc = &ha->dns_wc_head;
        keys = &ha->dns_wc_head_hash[k];

    } else { // 处理后置通配符

        /* convert "www.example.*" to "www.example\0" */

        last++;

        p = ngx_pnalloc(ha->temp_pool, last);
        if (p == NULL) {
            return NGX_ERROR;
        }

        ngx_cpystrn(p, key->data, last);

        hwc = &ha->dns_wc_tail;
        keys = &ha->dns_wc_tail_hash[k];
    }


    /* check conflicts in wildcard hash */

    /*
     * 检查带统配符的key是否冲突 比如.jd.com 和 *.jd.com 视为冲突
     * 		server{
     * 			listen 80;
     * 			server_name .jd.com;
     * 		}
     *
     * 		server{
     * 			listen 80;
     * 			server_name *.jd.com;
     * 		}
     * 上面两种会忽略一种,目前的逻辑是忽略"*.jd.com"对应的server
     */
    name = keys->elts;

    if (name) { // 第k个桶已经存在
        len = last - skip;

        for (i = 0; i < keys->nelts; i++) {
            if (len != name[i].len) {
                continue;
            }

            if (ngx_strncmp(key->data + skip, name[i].data, len) == 0) {
                return NGX_BUSY;
            }
        }

    } else { // 第k个桶还不存在
        if (ngx_array_init(keys, ha->temp_pool, 4, sizeof(ngx_str_t)) != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    name = ngx_array_push(keys);
    if (name == NULL) {
        return NGX_ERROR;
    }

    name->len = last - skip;
    name->data = ngx_pnalloc(ha->temp_pool, name->len);
    if (name->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(name->data, key->data + skip, name->len);


    /* add to wildcard hash */

    /*
     * 向hwc数组中放一个ngx_hash_key_t对象,为后续创建hash结构做准备
     */
    hk = ngx_array_push(hwc);
    if (hk == NULL) {
        return NGX_ERROR;
    }

    // 去掉通配符的长度
    hk->key.len = last - 1;
    hk->key.data = p;
    hk->key_hash = 0;
    hk->value = value;

    return NGX_OK;
}
