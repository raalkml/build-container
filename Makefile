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
