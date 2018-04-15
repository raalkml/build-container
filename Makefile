CPPFLAGS = -D_GNU_SOURCE -D'CONTAINER_DIR="."'
CFLAGS = -ggdb -O0
run-build-container: run-build-container.c

.PHONY: test
test:
	BUILD_CONTAINER_PATH=$(abspath .) ./run-build-container -n example -c
	BUILD_CONTAINER_PATH= ./run-build-container -n example -c || true
	BUILD_CONTAINER_PATH=: ./run-build-container -n NONE -c |grep config.file
	BUILD_CONTAINER_PATH=~ ./run-build-container -n NONE -c |grep config.file
	BUILD_CONTAINER_PATH=/etc/:~ ./run-build-container -n NONE -c |grep config.file
	BUILD_CONTAINER_PATH=/usr/etc:~/.config ./run-build-container -n NONE -c |grep config.file
	BUILD_CONTAINER_PATH=~/.config:/etc ./run-build-container -n NONE -c |grep config.file


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
