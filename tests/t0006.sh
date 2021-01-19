#!/bin/sh

# Verify overlay and union default options

echo '
from a
from b
work w
to m
overlay
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x0 0x0 'index=off,xino=off,upperdir="

echo '
from a
from b
to m
union
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x0 0x0 'xino=off,lowerdir="

# Verify overlay and union default options  are not affected by mount(2) flags

echo '
from a
from b
work w
to m
overlay ro
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x1 0x0 'index=off,xino=off,upperdir="

echo '
from a
from b
to m
union ro
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x1 0x0 'xino=off,lowerdir="

