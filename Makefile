override TOP_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
export TOP_MAKEFILE_DIR := $(dir $(TOP_MAKEFILE))

CPPFLAGS = -D_GNU_SOURCE
CFLAGS = -ggdb -O2 -pedantic -Wall

DESTDIR=
PREFIX=/usr/local/bin

run-build-container: run-build-container.c
clean:
	$(RM) run-build-container.o run-build-container

.PHONY:
install:
	install -m4755 -oroot run-build-container '$(DESTDIR)$(PREFIX)/'

.PHONY: test tests FORCE
test: $(sort $(wildcard tests/t[0-9][0-9][0-9][0-9]*.sh))


TEST_VERBOSE := false
TEST_AUTOCLEAN := true
TEST_SRC_DIR := $(abspath $(TOP_MAKEFILE_DIR))
TEST_DIR := $(TOP_MAKEFILE_DIR)/t
TEST_PATH := $(TEST_SRC_DIR):${PATH}
export TEST_SRC_DIR TEST_DIR TEST_PATH TEST_VERBOSE

tests/t%.sh: FORCE
	@echo; echo Running \"$@\"; echo
	@if $(TEST_AUTOCLEAN);then rm -rf "$(abspath $(TEST_DIR))/$(@F)";fi;mkdir -p "$(abspath $(TEST_DIR))/$(@F)"
	. $(@D)/common.sh; cd "$(abspath $(TEST_DIR))/$(@F)" && exec "$(abspath $@)" < /dev/null
	@$(TEST_AUTOCLEAN) && rm -rf "$(abspath $(TEST_DIR))/$(@F)"
