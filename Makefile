CPPFLAGS = -D_GNU_SOURCE -D'CONTAINER_DIR="."'
CFLAGS = -ggdb -O0

DESTDIR=
PREFIX=/usr/local/bin

run-build-container: run-build-container.c

.PHONY:
install:
	install -m4755 -oroot run-build-container '$(DESTDIR)$(PREFIX)/'

.PHONY: test
test:
	mkdir -p t/mnt1 t/mnt2 t/mnt3 t/mnt4
	./run-build-container -n $(abspath example) -c >t/result
	grep config.file.\'$(abspath example)\' t/result
	./run-build-container -n $(abspath example) -d t -c >t/result
	grep "# cd 't'" t/result
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
	mkdir -p $(addprefix $(@D)/,bin mnt mnt1 mnt2 mnt3)
	echo '#!/bin/sh' >$(@D)/mnt/gzip
	echo 'echo Not a gzip' >>$(@D)/mnt/gzip
	chmod +x $(@D)/mnt/gzip
	exec >$@; \
	echo 'from mnt'; \
	echo 'from /bin'; \
	echo 'to bin'; \
	echo 'union'; \
	echo 'from bin'; \
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
	mkdir -p $(addprefix $(@D)/,bin mnt mnt1 mnt2 mnt3)
	echo '#!/bin/sh' >$(@D)/mnt/gzip
	echo 'echo Not a gzip' >>$(@D)/mnt/gzip
	chmod +x $(@D)/mnt/gzip
	exec >$@; \
	echo 'from mnt/gzip'; \
	echo 'to /bin/gzip'; \
	echo 'bind'
	BUILD_CONTAINER_PATH=$(abspath $(@D)) ./run-build-container -n $(@F) -c
	sudo env BUILD_CONTAINER_PATH=$(abspath $(@D)) \
	    ./run-build-container -n $(@F) -e gzip | grep Not.a.gzip

.PHONY: t/sudo-test3.conf
t/sudo-test3.conf:
	mkdir -p $(addprefix $(@D)/,mnt)
	exec >$@; \
	echo 'from /usr'; \
	echo 'to mnt'; \
	echo 'bind'
	./run-build-container -n $(abspath $@) -c
	sudo ./run-build-container -n $(abspath $@) -d $(@D)/mnt -e ls -- -d bin \
	    |grep '^bin$$'

sudo-test: t/sudo-test1.conf t/sudo-test2.conf t/sudo-test3.conf
	mkdir -p t/mnt4
	# no pid namespace
	sleep 2 & pid=$$!; \
	sudo BUILD_CONTAINER_PATH=$(abspath .) ./run-build-container -n example -e kill -- -0 $$pid
	# pid namespace, parent proc fs
	sleep 2 & pid=$$!; \
	test $$(sudo BUILD_CONTAINER_PATH=$(abspath .) ./run-build-container -n example -e cat -P -- /proc/$$pid/comm) = sleep
	test $$(sudo BUILD_CONTAINER_PATH=$(abspath .) ./run-build-container -n example -P -- -c 'echo $$$$') = 1
	# pid namespace, own proc fs
	sudo BUILD_CONTAINER_PATH=$(abspath .) ./run-build-container -n example -e grep -PP -- \
	    '^PPid:[[:space:]]\+0[[:space:]]*$$' /proc/self/status
	# network namespace, loopback up
	sudo BUILD_CONTAINER_PATH=$(abspath .) sh -c 'set -x; PATH="$(abspath .):$$PATH"; \
	run-build-container -n example -N -e ping -- -nc1 127.0.0.1 || exit 1; \
	run-build-container -n example -P -N -e ping -- -nc1 127.0.0.1 || exit 1; \
	run-build-container -n example -PP -N -e ping -- -nc1 127.0.0.1 || exit 1; \
	if [ $$(ip -oneline link |wc -l) = 1 ]; then \
	test $$(run-build-container -n example -qN -e ip -- -oneline link |wc -l) = 1 || exit 1; \
	fi; \
	if [ $$(ss -Hatux |wc -l) -gt 0 ]; then \
	test $$(run-build-container -n example -qN -e ss -- -Hatux |wc -l) = 0 || exit 1; \
	fi; \
	exit 0'
