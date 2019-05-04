
default:	build

clean:
	rm -rf Makefile objs

build:
	$(MAKE) -f objs/Makefile
	$(MAKE) -f objs/Makefile manpage

install:
	$(MAKE) -f objs/Makefile install

upgrade:
	/My/workspace/nginx/sbin/nginx -t

	kill -USR2 `cat /My/workspace/nginx/logs/nginx.pid`
	sleep 1
	test -f /My/workspace/nginx/logs/nginx.pid.oldbin

	kill -QUIT `cat /My/workspace/nginx/logs/nginx.pid.oldbin`
