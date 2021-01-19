#!/bin/sh

# user namespace test

mkdir a b
echo '
to b
mount tmpfs
' | "$TEST_SRC_DIR/run-build-container" -n- -e sh -- -c 'echo a > b/b; ls -la b/b'

echo '
from a
to b
bind
' | "$TEST_SRC_DIR/run-build-container" -n- -e sh -- -c 'echo a > a/a; ls -la b/a'
