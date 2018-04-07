
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

/**
 * 队列(用双链表实现)
 *
 * 该容器不负责分配任何内存，每个元素需自行管理自己的内存使用
 *
 * 例子：
 *  创建一个容器并初始化
 *  ngx_queue_t container;
 *  ngx_queue_init(&container);
 *
 *  此后container就代表容器的sentinel(哨兵),也就是容器本身.
 *
 *
 *  任何一个想使用本队列的结构体，只要他们本身包含一个ngx_queue_t就可以,例如:
 *  typedef struct {
 *      ngx_queue_t nqt;
 *
 *      u_char *name;
 *      int age;
 *  } my_node;
 *
 *
 *  之后就可以以容器container对象为入参来使用,nginx为队列提供的各种方法了,比如:
 *  my_node node;
 *  node.name = "张三";
 *  node.age = 15;
 *
 *  ngx_queue_insert_head(&container,&node.nqt);将node添加到队列头部
 *
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>


#ifndef _NGX_QUEUE_H_INCLUDED_
#define _NGX_QUEUE_H_INCLUDED_


typedef struct ngx_queue_s  ngx_queue_t;

struct ngx_queue_s {
    ngx_queue_t  *prev;
    ngx_queue_t  *next;
};


/**
 * 初始化队列
 * q: 一个指向ngx_queue_t结构体的指针
 */
#define ngx_queue_init(q)                                                     \
    (q)->prev = q;                                                            \
    (q)->next = q


/**
 * 判断队列是否为空
 * 只要本身和它指向的前一个元素地址相等则表示队列为空
 *
 * h: 链表中的一个元素或者容器本身
 */
#define ngx_queue_empty(h)                                                    \
    (h == (h)->prev)


/**
 * 将x插入到容器h的头部
 *
 * h: 容器本身的指针;如果h不是容器本身,则代表将元素x插入到元素h之后
 * x: 要插入的元素
 */
#define ngx_queue_insert_head(h, x)                                           \
    (x)->next = (h)->next;                                                    \
    (x)->next->prev = x;                                                      \
    (x)->prev = h;                                                            \
    (h)->next = x


/**
 * 将元素x插入到元素h之后
 * h: 链表中某个元素
 * x: 要插入的元素
 */
#define ngx_queue_insert_after   ngx_queue_insert_head


/**
 * 将x插入到链表h的尾部
 *
 * h: 可以是容器本身的指针，也可以是一个普通的ngx_queue_t结构体指针
 * x: 要插入的元素
 */
#define ngx_queue_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x


/**
 * 获取容器的第一个元素
 *
 * h: 容器本身的指针
 *  容器本身的指针本身不代表任何元素,只是一个sentinel(哨兵),所以容器的next指向的就是链表的第一个元素
 */
#define ngx_queue_head(h)                                                     \
    (h)->next


/**
 * 获取容器的最后一个元素
 *
 * h: 容器本身的指针
 *  容器本身的指针本身不代表任何元素,只是一个sentinel(哨兵),容器prev指向的就是链表的最后一个元素
 */
#define ngx_queue_last(h)                                                     \
    (h)->prev


/**
 * 获取容器本身的指针;  //TODO 这个宏有啥意义,既然已经知道指针本身了,在调用这个宏意义何在?
 * h: 不代表容器本身的指针吗? //TODO
 */
#define ngx_queue_sentinel(h)                                                 \
    (h)


/**
 * 返回链表元素q的下一个元素,如果返回容器本身(sentinel)则代表结束
 *
 * q: 容器中的一个元素
 *
 */
#define ngx_queue_next(q)                                                     \
    (q)->next


/**
 * 返回链表元素q的上一个元素,如果返回容器本身(sentinel)则代表结束
 *
 * q: 容器中的一个元素
 *
 */
#define ngx_queue_prev(q)                                                     \
    (q)->prev


#if (NGX_DEBUG)

/**
 * 将元素x从链表中移除
 *
 * x: 容器中的一个元素
 */
#define ngx_queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next;                                              \
    (x)->prev = NULL;                                                         \
    (x)->next = NULL

#else

/**
 * 将元素x从链表中移除
 *
 * x: 容器中的一个元素
 */
#define ngx_queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next

#endif


/**
 * 将容器h以q为分界点,拆为两个队列
 *
 * h: 容器本身的指针
 * q: 容器h中的一个元素,拆完后q不再属于容器h,而属于新的容器
 * n: 新容器本身的指针,n中原来的元素被抛弃
 */
#define ngx_queue_split(h, q, n)                                              \
    (n)->prev = (h)->prev;                                                    \
    (n)->prev->next = n;                                                      \
    (n)->next = q;                                                            \
    (h)->prev = (q)->prev;                                                    \
    (h)->prev->next = h;                                                      \
    (q)->prev = n;


/**
 * 容器合并
 * 将容器n中的元素添加到容器h的末尾
 *
 * h: 一个容器指针
 * n: 一个容器指针
 *
 */
#define ngx_queue_add(h, n)                                                   \
    (h)->prev->next = (n)->next;                                              \
    (n)->next->prev = (h)->prev;                                              \
    (h)->prev = (n)->prev;                                                    \
    (h)->prev->next = h;


/**
 * 获取元素q关联的用户数据结构体指针
 *
 * q: 容器链表中的一个元素(ngx_queue_t)指针
 * type: 包含q的用户数据结构体,比如结构体my_node
 * link: q在用户结构体my_node中的名字(nqt)
 *
 */
#define ngx_queue_data(q, type, link)                                         \
    (type *) ((u_char *) q - offsetof(type, link))


/**
 * 返回容器中间元素
 */
ngx_queue_t *ngx_queue_middle(ngx_queue_t *queue);
/**
 * 对容器中的元素排序
 *
 *  *queue: 容器本身指针
 *  *cmp: 比较方法
 */
void ngx_queue_sort(ngx_queue_t *queue,
    ngx_int_t (*cmp)(const ngx_queue_t *, const ngx_queue_t *));


#endif /* _NGX_QUEUE_H_INCLUDED_ */
