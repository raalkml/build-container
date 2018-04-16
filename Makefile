CPPFLAGS = -D_GNU_SOURCE -D'CONTAINER_DIR="."'
CFLAGS = -ggdb -O0
run-build-container: run-build-container.c

.PHONY: test
test:
	mkdir -p t/mnt1 t/mnt2 t/mnt3 t/mn4
	./run-build-container -n $(abspath example) -c >t/result
	grep config.file.\'$(abspath example)\' t/result
	BUILD_CONTAINER_PATH=$(abspath .) ./run-build-container -n example -c >/dev/null
	LANG=C BUILD_CONTAINER_PATH= ./run-build-container -n example -c 2>t/result || true
	grep 'No defined path for configuration file example' t/result
	LANG=C BUILD_CONTAINER_PATH=: ./run-build-container -n NONE -c >t/result; test $$? = 3
	grep 'config.file.*/NONE.*No such file' t/result
	LANG=C BUILD_CONTAINER_PATH=~ ./run-build-container -n NONE -c >t/result; test $$? = 3
	grep 'config.file.*/NONE.*No such file' t/result
	LANG=C BUILD_CONTAINER_PATH=/etc/:~ ./run-build-container -n NONE -c >t/result; test $$? = 3
	grep 'config.file.*/NONE.*No such file' t/result
	LANG=C BUILD_CONTAINER_PATH=/usr/etc:~/.config ./run-build-container -n NONE -c; test $$? = 3
	grep 'config.file.*/NONE.*No such file' t/result
	LANG=C BUILD_CONTAINER_PATH=~/.config:/etc ./run-build-container -n NONE -c; test $$? = 3
	grep 'config.file.*/NONE.*No such file' t/result


.PHONY: t/sudo-test1.conf
t/sudo-test1.conf:
	mkdir -p $(@D)/bin $(@D)/mnt
	echo '#!/bin/sh' >$(@D)/mnt/gzip
	echo 'echo Not a gzip' >>$(@D)/mnt/gzip
	chmod +x $(@D)/mnt/gzip
	exec >$@; \
	echo 'from $(@D)/mnt'; \
	echo 'from /bin'; \
	echo 'to $(@D)/bin'; \
	echo 'union'; \
	echo 'from $(@D)/bin'; \
	echo 'to /bin'; \
	echo 'move'; \
	echo 'from $(abspath .)'; \
	echo 'to /usr/src'; \
	echo 'bind'
	BUILD_CONTAINER_PATH=$(abspath $(@D)) ./run-build-container -n $(@F) -c
	sudo env BUILD_CONTAINER_PATH=$(abspath $(@D)) \
	    ./run-build-container -n $(@F) -e gzip | grep Not.a.gzip
	sudo env BUILD_CONTAINER_PATH=$(abspath $(@D)) \
	    ./run-build-container -n $(@F) -e ls -- /usr/src |grep run-build-container

.PHONY: t/sudo-test2.conf
t/sudo-test2.conf:
	mkdir -p $(@D)/bin $(@D)/mnt
	echo '#!/bin/sh' >$(@D)/mnt/gzip
	echo 'echo Not a gzip' >>$(@D)/mnt/gzip
	chmod +x $(@D)/mnt/gzip
	exec >$@; \
	echo 'from $(@D)/mnt/gzip'; \
	echo 'to /bin/gzip'; \
	echo 'bind'
	BUILD_CONTAINER_PATH=$(abspath $(@D)) ./run-build-container -n $(@F) -c
	sudo env BUILD_CONTAINER_PATH=$(abspath $(@D)) \
	    ./run-build-container -n $(@F) -e gzip | grep Not.a.gzip

sudo-test: t/sudo-test1.conf t/sudo-test2.conf
