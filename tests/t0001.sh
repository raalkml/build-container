#!/bin/sh

mkdir -p t/bin t/mnt t/mnt1 t/mnt2 t/mnt3
echo '#!/bin/sh' >t/mnt/gzip
echo 'echo Not a gzip' >>t/mnt/gzip
chmod +x t/mnt/gzip

echo "
from t/mnt
from /bin
to t/bin
union

from t/bin
to /bin
move

from $TEST_SRC_DIR
to /usr/src
bind
" > sudo-test

BUILD_CONTAINER_PATH=$(pwd) $DEBUGGER run-build-container -n sudo-test -c || exit 1

sudo env BUILD_CONTAINER_PATH=$(pwd) \
    "$TEST_SRC_DIR/run-build-container" -n sudo-test -e gzip |
grep Not.a.gzip || exit 1

sudo env BUILD_CONTAINER_PATH=$(pwd) \
    "$TEST_SRC_DIR/run-build-container" -n sudo-test -e ls -- /usr/src |
grep run-build-container || exit 1
