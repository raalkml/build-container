#!/bin/sh

test -n "$TEST_PATH" && PATH="$TEST_PATH"
case "$TEST_VERBOSE" in [YyTt1]*) ;; *) exec >/dev/null ;; esac

