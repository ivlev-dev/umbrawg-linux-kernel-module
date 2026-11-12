# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.

WIREGUARD_VERSION = 1.0.0
OMIT_ENDPOINTS ?=
UWG_NAME ?= umbrawg

KERNELRELEASE ?= $(shell uname -r)
KERNELDIR ?= /lib/modules/$(KERNELRELEASE)/build
PREFIX ?= /usr
DESTDIR ?=
SRCDIR ?= $(PREFIX)/src
DKMSDIR ?= $(SRCDIR)/umbrawg-$(WIREGUARD_VERSION)
DEPMOD ?= depmod
DEPMODBASEDIR ?= /

PWD := $(shell pwd)

ifeq ($(LLVM),)
ifneq ($(shell test -f $(KERNELDIR)/.config && grep -qs '^CONFIG_CC_IS_CLANG=y' $(KERNELDIR)/.config && echo yes),)
LLVM := 1
endif
endif

KMAKE = $(MAKE) -C $(KERNELDIR) M=$(PWD) \
	UWG_NAME="$(UWG_NAME)" \
	$(if $(LLVM),LLVM=$(LLVM),) \
	WIREGUARD_VERSION="$(WIREGUARD_VERSION)" \
	OMIT_ENDPOINTS="$(OMIT_ENDPOINTS)"

all: module
debug: module-debug

ifneq ($(V),1)
MAKEFLAGS += --no-print-directory
endif

module:
	@$(KMAKE) modules

module-debug:
	@$(KMAKE) V=1 CONFIG_UMBRAWG_DEBUG=y modules

clean:
	@$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

module-install:
	@$(KMAKE) modules_install
	$(DEPMOD) -b "$(DEPMODBASEDIR)" -a $(KERNELRELEASE)

install: module-install

rwildcard=$(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2) $(filter $(subst *,%,$2),$d))
DKMS_SOURCES := version.h Makefile Kbuild Kconfig dkms.conf uwg_identity.h $(filter-out version.h wireguard.mod.c tests/% uwg_identity.h,$(call rwildcard,,*.c *.h *.S *.include))
dkms-install: $(DKMS_SOURCES)
	@$(foreach f,$(DKMS_SOURCES),install -v -m0644 -D $(f) $(DESTDIR)$(DKMSDIR)/$(f);)

style:
	$(KERNELDIR)/scripts/checkpatch.pl -f --max-line-length=4000 --codespell --color=always $(filter-out wireguard.mod.c,$(wildcard *.c)) $(wildcard *.h) $(wildcard selftest/*.c)

check: clean
	scan-build --html-title=umbrawg -maxloop 100 --view --keep-going $(MAKE) module CONFIG_UMBRAWG_DEBUG=y C=2 CF="-D__CHECK_ENDIAN__"

coccicheck: clean
	@$(MAKE) -C $(KERNELDIR) M=$(PWD) CONFIG_UMBRAWG_DEBUG=y coccicheck MODE=report

-include tests/debug.mk

.PHONY: all module module-debug module-install install dkms-install clean check style
