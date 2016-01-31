
default:	build

clean:
	rm -rf Makefile objs

build:
	$(MAKE) -f objs/Makefile
	$(MAKE) -f objs/Makefile manpage

install:
	$(MAKE) -f objs/Makefile install

upgrade:
	/export/servers/nginx/sbin/nginx -t

	kill -USR2 `cat /export/servers/nginx/logs/nginx.pid`
	sleep 1
	test -f /export/servers/nginx/logs/nginx.pid.oldbin

	kill -QUIT `cat /export/servers/nginx/logs/nginx.pid.oldbin`
