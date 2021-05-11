#!/bin/sh
mkdir -p t/mnt t/src
echo MARK >t/src/mark
echo 'from t/src
to t/mnt
bind
' >config

(cd t/src/; ls) > orig
$DEBUGGER run-build-container -n $(realpath config) -d t/mnt -e ls >result-d
diff -u orig result-d
$DEBUGGER run-build-container -n $(realpath config) -w t/mnt -e ls >result-w
diff -u orig result-w

