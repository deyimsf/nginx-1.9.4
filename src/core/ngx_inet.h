
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_INET_H_INCLUDED_
#define _NGX_INET_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


/*
 * TODO: autoconfigure NGX_SOCKADDRLEN and NGX_SOCKADDR_STRLEN as
 *       sizeof(struct sockaddr_storage)
 *       sizeof(struct sockaddr_un)
 *       sizeof(struct sockaddr_in6)
 *       sizeof(struct sockaddr_in)
 */

#define NGX_INET_ADDRSTRLEN   (sizeof("255.255.255.255") - 1)
#define NGX_INET6_ADDRSTRLEN                                                 \
    (sizeof("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255") - 1)
#define NGX_UNIX_ADDRSTRLEN                                                  \
    (sizeof(struct sockaddr_un) - offsetof(struct sockaddr_un, sun_path))

#if (NGX_HAVE_UNIX_DOMAIN)
#define NGX_SOCKADDR_STRLEN   (sizeof("unix:") - 1 + NGX_UNIX_ADDRSTRLEN)
#else
#define NGX_SOCKADDR_STRLEN   (NGX_INET6_ADDRSTRLEN + sizeof("[]:65535") - 1)
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
#define NGX_SOCKADDRLEN       sizeof(struct sockaddr_un)
#else
#define NGX_SOCKADDRLEN       512
#endif


typedef struct {
    in_addr_t                 addr;
    in_addr_t                 mask;
} ngx_in_cidr_t;


#if (NGX_HAVE_INET6)

typedef struct {
    struct in6_addr           addr;
    struct in6_addr           mask;
} ngx_in6_cidr_t;

#endif


typedef struct {
    ngx_uint_t                family;
    union {
        ngx_in_cidr_t         in;
#if (NGX_HAVE_INET6)
        ngx_in6_cidr_t        in6;
#endif
    } u;
} ngx_cidr_t;


typedef struct {
	// 套接字地址,包括端口号
    struct sockaddr          *sockaddr;
    socklen_t                 socklen;
    // 字符串形式的IP地址(如 192.168.1.1:8001)
    ngx_str_t                 name;
} ngx_addr_t;


typedef struct {
	/*
	 * 可以是upstream中server指令指定的,比如
	 * 		server 127.0.0.1:8080
	 * 		server www.jd.com
	 *
	 */
    ngx_str_t                 url;

    /*
     * 可以是upstream tomcat {}中的tomcat
     *
     * 可以是upsteam{}中server指令指定的值,比如
     * 		server 127.0.0.1:8080
     * 		server www.jd.com
     *
     */
    ngx_str_t                 host;

    /*
     * 文本形式的端口号,比如"8080"
     */
    ngx_str_t                 port_text;
    ngx_str_t                 uri;

    /*
     * 数字形式的端口号,比如8080
     */
    in_port_t                 port;
    /*
     * 默认端口号80
     */
    in_port_t                 default_port;
    int                       family;

    unsigned                  listen:1;
    unsigned                  uri_part:1;
    unsigned                  no_resolve:1;
    unsigned                  one_addr:1;  /* compatibility */

    /* 表示这个url没有指明端口号 */
    unsigned                  no_port:1;
    unsigned                  wildcard:1;

    /* host对应ip的socket长度 */
    socklen_t                 socklen;
    /* host对应ip的socket信息 */
    u_char                    sockaddr[NGX_SOCKADDRLEN];

    /*
     * 套接字地址,包括端口
     * addrs->sockaddr中的值和上面字段sockaddr值相同
     * 		print *(struct sockaddr*)&u.sockaddr
     * 		print *u.addrs->sockaddr
     *
     * addrs->socklen和socklen值相同
     *
     */
    ngx_addr_t               *addrs;
    ngx_uint_t                naddrs;

    char                     *err;
} ngx_url_t;


in_addr_t ngx_inet_addr(u_char *text, size_t len);
#if (NGX_HAVE_INET6)
ngx_int_t ngx_inet6_addr(u_char *p, size_t len, u_char *addr);
size_t ngx_inet6_ntop(u_char *p, u_char *text, size_t len);
#endif
size_t ngx_sock_ntop(struct sockaddr *sa, socklen_t socklen, u_char *text,
    size_t len, ngx_uint_t port);
size_t ngx_inet_ntop(int family, void *addr, u_char *text, size_t len);
ngx_int_t ngx_ptocidr(ngx_str_t *text, ngx_cidr_t *cidr);
ngx_int_t ngx_parse_addr(ngx_pool_t *pool, ngx_addr_t *addr, u_char *text,
    size_t len);
ngx_int_t ngx_parse_url(ngx_pool_t *pool, ngx_url_t *u);
ngx_int_t ngx_inet_resolve_host(ngx_pool_t *pool, ngx_url_t *u);
ngx_int_t ngx_cmp_sockaddr(struct sockaddr *sa1, socklen_t slen1,
    struct sockaddr *sa2, socklen_t slen2, ngx_uint_t cmp_port);


#endif /* _NGX_INET_H_INCLUDED_ */
