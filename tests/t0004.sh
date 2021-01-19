#!/bin/sh

mkdir -p t/mnt1 t/mnt2 t/mnt3 t/mnt6
mkdir -p t/mnt4/mnt t/mnt4/top t/mnt4/bottom t/mnt4/wrk t/mnt5/mnt t/mnt5/top t/mnt5/bottom t/mnt5/wrk
mkdir -p t/union-top t/union-bottom
echo TOP >t/union-top/file
echo BOTTOM >t/union-bottom/file

cp "$TEST_SRC_DIR/example" . || exit 2

# no pid namespace
sleep 2 &
pid=$!
sudo BUILD_CONTAINER_PATH=$(pwd) "$TEST_SRC_DIR/run-build-container" -n example -e kill -- -0 $pid
rc=$?
kill $pid
wait
test $rc = 0 || exit 1

# pid namespace, parent proc fs
sleep 2 &
pid=$!
test $(sudo BUILD_CONTAINER_PATH=$(pwd) "$TEST_SRC_DIR/run-build-container" -n example -e cat -P -- /proc/$pid/comm) = sleep
rc1=$?
test $(sudo BUILD_CONTAINER_PATH=$(pwd) "$TEST_SRC_DIR/run-build-container" -n example -P -- -c 'echo $$') = 1
rc2=$?
kill $pid
wait
test $rc1 = 0 || exit 1
test $rc2 = 0 || exit 1

# pid namespace, own proc fs
sudo BUILD_CONTAINER_PATH=$(pwd) "$TEST_SRC_DIR/run-build-container" -n example -e grep -PP -- \
    '^PPid:[[:space:]]\+0[[:space:]]*$' /proc/self/status
test $? = 0 || exit 1

# network namespace, loopback up
sudo BUILD_CONTAINER_PATH=$(pwd) PATH="$TEST_SRC_DIR:$PATH" sh -c '
run-build-container -n example -N -e ping -- -nc1 127.0.0.1 || exit 1
run-build-container -n example -P -N -e ping -- -nc1 127.0.0.1 || exit 1
run-build-container -n example -PP -N -e ping -- -nc1 127.0.0.1 || exit 1
if [ $(ip -oneline link |wc -l) = 1 ]; then
  test $(run-build-container -n example -qN -e ip -- -oneline link |wc -l) = 1 || exit 1
fi
if [ $(ss -Hatux |wc -l) -gt 0 ]; then
  test $(run-build-container -n example -qN -e ss -- -Hatux |wc -l) = 0 || exit 1
fi
exit 0'
