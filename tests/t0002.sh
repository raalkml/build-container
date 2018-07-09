#!/bin/sh

mkdir -p t/bin t/mnt t/mnt1 t/mnt2 t/mnt3
echo '#!/bin/sh' >t/mnt/gzip
echo 'echo Not a gzip' >>t/mnt/gzip
chmod +x t/mnt/gzip

echo '
from t/mnt/gzip
to /bin/gzip
bind
' >sudo-test

BUILD_CONTAINER_PATH=$(pwd) $DEBUGGER run-build-container -n sudo-test -c

sudo env BUILD_CONTAINER_PATH=$(pwd) \
    "$TEST_SRC_DIR/run-build-container" -n sudo-test -e gzip | grep Not.a.gzip

