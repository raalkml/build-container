#!/bin/sh

#
# Validate overlay and union option parsing
#

echo '
from a
from b
work w
to m
overlay ro xino=off
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x1 'xino=off,upperdir="

echo '
from a
from b
work w
to m
overlay xino=off ro
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x1 'xino=off,upperdir="

echo '
from a
from b
to m
union rec xino=off ro
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x4001 'xino=off,lowerdir="

echo '
from a
from b
to m
union xino=off noexec rec
' >tst
run-build-container -c -n $(pwd)/tst |grep "m' overlay 0x4008 'xino=off,lowerdir="
