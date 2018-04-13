CPPFLAGS = -D_GNU_SOURCE -D'CONTAINER_DIR="."'
CFLAGS = -ggdb -O0
run-build-container: run-build-container.c

.PHONY: test
test:
	sudo env PATH="$$PATH" ./run-build-container -n example \
	    -e /bin/sh -- -c 'ls -l --color mnt*; cat mnt3/file'
