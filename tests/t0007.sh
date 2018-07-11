#!/bin/sh

echo '
from a
to b
bind
' | run-build-container -n- -c || exit 1

mkdir a b
echo '
from a
to b
bind
' | sudo "$TEST_SRC_DIR/run-build-container" -n- -e sh -- -c 'echo a > b/b' || exit 1
test $(cat a/b) = a
