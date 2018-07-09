#!/bin/sh
PATH="$TEST_PATH"
case "$TEST_VERBOSE" in [YyTt1]*) ;; *) exec >/dev/null ;; esac

mkdir -p t/mnt1 t/mnt2 t/mnt3
mkdir -p t/mnt4/mnt t/mnt4/top t/mnt4/bottom t/mnt4/wrk t/mnt5/mnt t/mnt5/top t/mnt5/bottom t/mnt5/wrk
mkdir -p t/union-top t/union-bottom
echo TOP >t/union-top/file
echo BOTTOM >t/union-bottom/file

run-build-container -n "$TEST_SRC_DIR/example" -c >t/result
grep "^# config.file.'$TEST_SRC_DIR/example'" t/result || exit 1

run-build-container -n "$TEST_SRC_DIR/example" -d t -c >t/result
grep "^# cd 't'" t/result || exit 1

BUILD_CONTAINER_PATH="$TEST_SRC_DIR" $DEBUGGER run-build-container -n example -c || exit 1

LANG=C BUILD_CONTAINER_PATH= $DEBUGGER run-build-container -n example -c 2>t/result
grep 'No defined path for configuration file example' t/result || exit 1

LANG=C BUILD_CONTAINER_PATH=: $DEBUGGER run-build-container -n NONE -c >t/result
test $? = 3 || exit 1
grep 'config.file.*/NONE.*No such file' t/result || exit 1

LANG=C BUILD_CONTAINER_PATH=~ $DEBUGGER run-build-container -n NONE -c >t/result
test $? = 3 || exit 1
grep 'config.file.*/NONE.*No such file' t/result || exit 1

LANG=C BUILD_CONTAINER_PATH=/etc/:~ $DEBUGGER run-build-container -n NONE -c >t/result
test $? = 3 || exit 1
grep 'config.file.*/NONE.*No such file' t/result || exit 1

LANG=C BUILD_CONTAINER_PATH=/usr/etc:~/.config $DEBUGGER run-build-container -n NONE -c
test $? = 3 || exit 1
grep 'config.file.*/NONE.*No such file' t/result || exit 1

LANG=C BUILD_CONTAINER_PATH=~/.config:/etc $DEBUGGER run-build-container -n NONE -c
test $? = 3 || exit 1
grep 'config.file.*/NONE.*No such file' t/result || exit 1

