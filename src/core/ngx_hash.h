
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/**
 * 散列表解决碰撞的方法
 * 1，链表法:把散列到同一槽中的所有元素都放在一个链表中
 *
 * 2，开放寻址法(再散列方法?):开放寻址发中所有的元素都存放在散列表中；
 * 	 当插入一个元素时,可以连续的检查散列表的各项，直到找到一个空槽来放置待插入的关键字为止。
 *
 * 	 插入操作描述如下:
 * 	 HASH-INSERT(T,k)
 * 	 i = 0
 * 	 repeat j = h(k,i)
 * 	 	if T[j] = null
 * 	 		T[j] = k
 * 	 		return j;
 * 	 	else
 * 	 		i = i + 1;
 * 	 until i = m;
 *
 * 	 error "hash table overflow";
 *
 * 	 其中T:散列表；k:要插入的元素; h(k,i):散列函数
 *
 * 3，完全散列：采用两级散列表
 *   第一级于链表散列基本一致；与链表发不同的是，不会对散列到某个槽(桶)上的的关键字建立一个链表，
 *   而是采用一个较小的二次散列表来存放发生碰撞的元素。
 *
 *
 * ngx中对散列表的实现算法基于开放寻址法,它有两块散列表(但不是完全散列方法),
 * 第一块散列表用来存放桶指针,这些指针用来指定确定每个桶在第二块表的起始位置.
 * 第二块表并不是一个散列表,就是一块连续的内存空间,这个空间里面存放了所有的元素,
 * 所以ngx中散列表解决碰撞的方式是数组的方式.
 *
 * 插入操作简单流程:
 *  0,先计算每个元素在每个桶的位置偏移量
 *	1,计算要插入元素的hash值,然后确定其桶的位置
 *	2,桶的起始位置 + 该元素在该桶中的偏移量 = elt
 *	3,将elt赋值 elt->value = 实际value的指针;
 *	           elt->name = key字符串的小写形式
 *
 *
 *  初始方法在开始的时候,会计算ngx_hash_elt_t结构体所需占用内存的总共大小,其中包括
 *  name的总长度 + len这个字段占用的字节个数(sizeof(u_short)) + 指针*value占用的字节数(sizeof(void *))
 *	但是并不包含 value 指向的内容长度。
 *
 */


#ifndef _NGX_HASH_H_INCLUDED_
#define _NGX_HASH_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef struct { //ngx_hash_t结构中实际存放的元素
	/*
	 * 键值对key对应的值value
	 */
    void             *value;

    u_short           len; //name的长度

    /*
     * 键值对(key-value)对应的键(key)
     * 直接用指针会占用4个字节，用组数就占一个字节(其实用数组name[0]更好,占零个字节)
     */
    u_char            name[1];
} ngx_hash_elt_t;


/**
 * 基本散列表结构体
 * 这个结构体才是真正意义上用来表示散列的结构体，其它都是对他的一个简单封装
 */
typedef struct {
    /*
     *      buckets
     *       -----
     *       | * |
     *       -----
     *         \
     *         ---------------------
     *         | * | * | * | * | size个桶
     *         ---------------------
     *          /        /    \
     *         -----------------------------------------
     *         |      好多个ngx_hash_elt_t              |
     *         -----------------------------------------
     * 可以看到第二个桶是空桶,其它桶不是空桶
     */
    ngx_hash_elt_t  **buckets;

    /*
     * hash桶实际个数
     */
    ngx_uint_t        size;
} ngx_hash_t;


/*
 * 支持通配符的散列表,只是对基本散列表做了一个简单的封装
 */
typedef struct {
	// 基本散列表
    ngx_hash_t        hash;
    // 用户可以使用这个指针来存储或传递一些东西
    void             *value;
} ngx_hash_wildcard_t;


//ngx_hash_t结构中的键值对
typedef struct {
	// 键值对中的键,ngx_hash_elt_t结构中的name来自这个key
    ngx_str_t         key;
    // key的hash值
    ngx_uint_t        key_hash;

    /*
     * 键值对中的原始值,ngx_hash_elt_t结构中的value指向该值
     *
	 * 在http变量中存放的是ngx_http_variable_t结构体(cmcf->variables_keys)
	 */
    void             *value;
} ngx_hash_key_t;


// 散列方法
typedef ngx_uint_t (*ngx_hash_key_pt) (u_char *data, size_t len);


/**
 * 支持通配符的散列表结构体
 * 该结构体包含三个散列表
 */
typedef struct {
	// 精确匹配的散列表
    ngx_hash_t            hash;
    // 匹配前置通配符的散列表,如：*.jd.com
    ngx_hash_wildcard_t  *wc_head;
    // 匹配后置通配符的散列表，如：www.jd.*
    ngx_hash_wildcard_t  *wc_tail;
} ngx_hash_combined_t;


/*
 * 只是在构造hash结构时使用
 * 当构造完*hash后就不需要了
 */
typedef struct {
	// 待初始化的hash结构,如果不指定则ngx_hash_init方法会自己创建该结构
    ngx_hash_t       *hash;
    // key的hash函数,用来计算key的hash值并确定桶的位置
    ngx_hash_key_pt   key;

    // 桶的最大个数,最终会根据实际情况计算出一个小于等于该值的数
    ngx_uint_t        max_size;
    // 每个桶所需要分配的字节个数
    ngx_uint_t        bucket_size;

    // hash结构的名字,打日志的时候使用
    char             *name;
    ngx_pool_t       *pool;
    ngx_pool_t       *temp_pool;
} ngx_hash_init_t;


/**
 * 决定散列表中桶的个数的类型,ngx_hash_keys_array_init方法用
 * 107个桶
 */
#define NGX_HASH_SMALL            1

/**
 * 决定散列表中桶的个数的类型,ngx_hash_keys_array_init方法用
 * NGX_HASH_LARGE_HSIZE个桶
 */
#define NGX_HASH_LARGE            2

// 数组初始元素个数
#define NGX_HASH_LARGE_ASIZE      16384
// 桶个数
#define NGX_HASH_LARGE_HSIZE      10007

#define NGX_HASH_WILDCARD_KEY     1
#define NGX_HASH_READONLY_KEY     2


/**
 * 用来帮助生成散列表的结构体。
 *
 * 包含三个散列表,基本散列表、前置通配符散列表、后置通配符散列表
 *
 * 该结构体最终会包含所有散列表的一份数据(通过ngx_hash_add_key方法添加的)
 *
 */
typedef struct {
	// 各个散列表的桶的个数
    ngx_uint_t        hsize;

    ngx_pool_t       *pool;
    ngx_pool_t       *temp_pool;

    /*
     * 以数组的形式存放基础散列表的键值对数据,以ngx_hash_key_t结构体存放
     * 由ngx_hash_add_key方法赋值
     *
     * 最终使用ngx_hash_init()方法用keys来创建hash结构
     */
    ngx_array_t       keys;
    /*
     * 简易散列表,数组中每一个元素代表一个桶,总共hsize个桶
     * 这个桶用ngx_array_t结构体表示,桶里面存储了不含通配符的域名
     *
     * 该字段的存在只是为了检查key值是否有冲突在ngx_hash_add_key()方法中用到,最终会使用上面的keys字段
     * 来构造hash结构,所以该字段中只放key的名字,其中带点的通配符字符串会被视为非通配符字符串,比如 jd.com
     * 和 .jd.com视为相同
     *
     * 这个机构类似于java中的HashMap,只不过是把链表换成数组
     * 			keys_hash
     * 			  -----
     * 			  | * |
     * 			  -----
     * 			    \
     * 			  ----------------------
     * 			  | * | * | * | * |  HashMap中的数组
     * 			  ----------------------
     *              /            \
     *    ----------------      ----------------
     *    | 多个ngx_str_t |      | 多个ngx_str_t |
     *    ----------------      ----------------
     *
     */
    ngx_array_t      *keys_hash;


    /*
     * 以数组的形式存放前置通配符散列表的键值对数据,以ngx_hash_key_t结构体存放
     * 由ngx_hash_add_key方法赋值
     * 如：*.jd.com
     *
     * 最终会使用ngx_hash_wildcard_init()方法用dns_wc_head来创建hash结构体
     */
    ngx_array_t       dns_wc_head;
    /*
     * 简易散列表,数组中每一个元素代表一个桶,总共hsize个桶
     * 这个桶用ngx_array_t结构体表示,桶里面存储了包含前置通配符的域名
     *
     * 用来检查前置通配符冲突
     */
    ngx_array_t      *dns_wc_head_hash;


    /*
     * 以数组的形式存放后置通配符散列表的键值对数据,以ngx_hash_key结构体存放
     * 由ngx_hash_add_key方法赋值
     * 如：www.jd.*
     * 最终会使用ngx_hash_wildcard_init()方法用dns_wc_head来创建hash结构体
     */
    ngx_array_t       dns_wc_tail;
    // 简易散列表,数组中每一个元素代表一个桶,总共hsize个桶
    // 这个桶用ngx_array_t结构体表示,桶里面存储了包含后置通配符的域名
    /*
     * 用来检查后置通配符冲突
     */
    ngx_array_t      *dns_wc_tail_hash;
} ngx_hash_keys_arrays_t;


typedef struct {
    ngx_uint_t        hash;
    ngx_str_t         key;
    ngx_str_t         value;
    u_char           *lowcase_key;
} ngx_table_elt_t;


/**
 * 从hash表中查找一个元素
 *
 * *hash: 散列表
 * key: 键值对中键的hash值
 * *name: 键值对中键的名字
 * len: 键名字的长度
 */
void *ngx_hash_find(ngx_hash_t *hash, ngx_uint_t key, u_char *name, size_t len);

void *ngx_hash_find_wc_head(ngx_hash_wildcard_t *hwc, u_char *name, size_t len);
void *ngx_hash_find_wc_tail(ngx_hash_wildcard_t *hwc, u_char *name, size_t len);
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
void *ngx_hash_find_combined(ngx_hash_combined_t *hash, ngx_uint_t key,
    u_char *name, size_t len);

/**
 * 初始化散列表,创建散列表并将names的数据赋值给散列表
 *
 * *hinit: 用于构造hash结构的临时结构体
 * *names: hash机构中要存入的键值对,该入参是个键值对数组
 * nelts: 键值对个数,也就是names的个数
 */
ngx_int_t ngx_hash_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts);

/**
 * 初始化带统配符的散列表
 *
 * *hinit: 用于构造hash结构的临时结构体
 * *names: hash机构中要存入的键值对,该入参是个键值对数组
 *		   全是带通配符的key,如 *.jd.com .jd.com www.jd.*
 * nelts: 键值对个数,也就是names的个数
 *
 */
ngx_int_t ngx_hash_wildcard_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts);

/**
 * ngx_hash_key方法中用到
 */
#define ngx_hash(key, c)   ((ngx_uint_t) key * 31 + c)

/**
 * 一个hash方法
 *
 * *data: 要hash的数据的指针
 * len: 要hash的数据的长度
 */
ngx_uint_t ngx_hash_key(u_char *data, size_t len);

/**
 * 一个hash方法，同ngx_hash_key方法
 * hash之前会将数据转换为小写
 *
 * *data: 要hash的数据的指针
 * len: 要hash的数据的长度
 */
ngx_uint_t ngx_hash_key_lc(u_char *data, size_t len);

/**
 * 将字符串 src 的小写形式赋值给字符串 dst,然后返回dst的hash值
 *
 * *src: 源字符串
 * *dst: 转换成小写形式的字符串
 *
 */
ngx_uint_t ngx_hash_strlow(u_char *dst, u_char *src, size_t n);


/**
 * 初始化ngx_hash_keys_arrays_t结构体
 *
 * *ha: 要初始化的ngx_hash_keys_arrays_t结构体
 * type: 决定散列表中桶的个数的类型
 *
 */
ngx_int_t ngx_hash_keys_array_init(ngx_hash_keys_arrays_t *ha, ngx_uint_t type);

/**
 * 向ngx_hash_keys_arrays_t中添加用户键值对
 *
 * *ha: ngx_hash_keys_arrays_t
 * *key: 用户key,比如“www.jd.com”、“.jd.com”、"*.jd.com"、“www.jd.*”
 * *value: 用户值,比如“198.168.12.32”
 * flags: 一个位掩码
 * 		NGX_HASH_WILDCARD_KEY: 处理通配符
 * 		NGX_HASH_READONLY_KEY: 不把key转换为小写
 *
 * 不允许存在相同的元素,如果存在返回NGX_BUSY
 *
 * (只为域名key服务,也就是说key不能是其它数据)
 */
ngx_int_t ngx_hash_add_key(ngx_hash_keys_arrays_t *ha, ngx_str_t *key,
    void *value, ngx_uint_t flags);


#endif /* _NGX_HASH_H_INCLUDED_ */
