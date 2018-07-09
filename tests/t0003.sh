#!/bin/sh
mkdir -p t/mnt
echo 'from /usr
to t/mnt
bind
' >sudo-test

$DEBUGGER run-build-container -n $(realpath sudo-test) -c

sudo "$TEST_SRC_DIR/run-build-container" -n $(realpath sudo-test) -d t/mnt -e ls -- -d bin \
    |grep '^bin$'

