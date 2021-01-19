#!/bin/sh

mkdir a b
echo '
to b
mount tmpfs
' | sudo "$TEST_SRC_DIR/run-build-container" -n- -e sh -- -c 'echo a > b/b; ls -la b/'
