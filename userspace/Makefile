# Userspace build helpers for Neutrino

SHELL := /bin/bash

CXX      ?= g++
CC       ?= gcc
AR       ?= ar
NASM     ?= nasm
SUDO     ?=
MOUNT_SUDO ?= sudo

CRT_DIR  := crt
LIBC_DIR := libc
HELPER_DIR := helpers
DEPS_DIR := deps
BEARSSL_DIR := $(DEPS_DIR)/BearSSL
OBJ_DIR  := build
SRC_PROG_DIR := programs
OUT_DIR  := out
BIN_DIR  := binary
LIB_DIR  := library
INCLUDE_DIR := include
CONFIG_DIR ?= config
DEPENDENCY_ROOT ?=
DEPENDENCY_INCLUDE_DIR ?= $(if $(DEPENDENCY_ROOT),$(DEPENDENCY_ROOT)/include,$(INCLUDE_DIR))
DEPENDENCY_LIBRARY_DIR ?= $(if $(DEPENDENCY_ROOT),$(DEPENDENCY_ROOT)/library,$(LIB_DIR))
NEWLIB_SOURCE_DIR ?= $(DEPS_DIR)/newlib-cygwin
NEWLIB_PORT_DIR := newlib-port
NEWLIB_BUILD_DIR := $(OBJ_DIR)/newlib-pie-build
NEWLIB_CONFIG_STAMP := $(NEWLIB_BUILD_DIR)/.neutrino-multithread-configured
NEWLIB_SYSROOT := $(OBJ_DIR)/newlib-pie-sysroot
NEWLIB_TARGET := x86_64-elf
NEWLIB_CC ?= $(NEWLIB_TARGET)-gcc
NEWLIB_AR ?= $(NEWLIB_TARGET)-ar
NEWLIB_RANLIB ?= $(NEWLIB_TARGET)-ranlib
NEWLIB_JOBS ?= 4
NEWLIB_INCLUDE_DIR := $(NEWLIB_SYSROOT)/$(NEWLIB_TARGET)/include
NEWLIB_PLATFORM_INCLUDE_DIR := $(NEWLIB_SYSROOT)/include
NEWLIB_LIBRARY_DIR := $(NEWLIB_SYSROOT)/$(NEWLIB_TARGET)/lib
NEWLIB_LIBC := $(NEWLIB_LIBRARY_DIR)/libc.a
NEWLIB_LIBM := $(NEWLIB_LIBRARY_DIR)/libm.a
NEWLIB_LIBNEUTRINO := $(NEWLIB_LIBRARY_DIR)/libneutrino.a
NEWLIB_CRT0 := $(NEWLIB_LIBRARY_DIR)/crt0.o
FFMPEG_SIBLING_SOURCE_DIR := $(abspath ../../neutrino-packages/ffmpeg/src)
FFMPEG_SOURCE_DIR ?= $(if $(wildcard $(FFMPEG_SIBLING_SOURCE_DIR)/configure),$(FFMPEG_SIBLING_SOURCE_DIR),$(DEPS_DIR)/ffmpeg)
FFMPEG_BUILD_DIR ?= $(OBJ_DIR)/ffmpeg-minimal
FFMPEG_THREAD_MODE ?= pthread
NEWLIB_PLATFORM_HEADER_NAMES := dirent.h poll.h pthread.h sched.h stdatomic.h sys/ioctl.h \
                               sys/mman.h time.h unistd.h
NEWLIB_PLATFORM_HEADERS := $(addprefix $(NEWLIB_PLATFORM_INCLUDE_DIR)/,$(NEWLIB_PLATFORM_HEADER_NAMES))
NEWLIB_PORT_OBJ_DIR := $(OBJ_DIR)/newlib-port
NEWLIB_PROGRAM_OBJ_DIR := $(OBJ_DIR)/newlib-programs
NEWLIB_PORT_OBJECTS := $(NEWLIB_PORT_OBJ_DIR)/start.o \
                       $(NEWLIB_PORT_OBJ_DIR)/syscalls.o \
                       $(NEWLIB_PORT_OBJ_DIR)/dirent.o \
                       $(NEWLIB_PORT_OBJ_DIR)/pthread.o \
                       $(NEWLIB_PORT_OBJ_DIR)/posix_compat.o
NEWLIB_CFLAGS := -O2 -g -ffreestanding -fno-builtin \
                 -fno-stack-protector -nostdlib -m64 -mno-red-zone \
                 -mno-avx -mno-avx512f -fPIE -D_POSIX_THREADS=200809L
NEWLIB_PORT_CFLAGS := $(NEWLIB_CFLAGS) -std=c11 -Wall -Wextra -Wpedantic \
                      -I$(NEWLIB_PORT_DIR)/include \
                      -isystem $(NEWLIB_INCLUDE_DIR) -I$(NEWLIB_PORT_DIR)
NEWLIB_PROGRAM_CFLAGS := $(NEWLIB_CFLAGS) -std=c11 -Wall -Wextra -Wpedantic \
                         -isystem $(NEWLIB_PLATFORM_INCLUDE_DIR) \
                         -isystem $(NEWLIB_INCLUDE_DIR)
NEWLIB_LINK_LIBS := -Wl,--start-group $(NEWLIB_LIBNEUTRINO) \
                    $(NEWLIB_LIBC) $(NEWLIB_LIBM) -lgcc -Wl,--end-group

# Every standalone C or C++ source in programs/ is built and installed into
# /binary. C sources use the Newlib CRT; legacy C++ sources retain the original
# runtime until they are migrated.
DEFAULT_CPP_PROGRAMS := $(patsubst $(SRC_PROG_DIR)/%.cpp,%,$(wildcard $(SRC_PROG_DIR)/*.cpp))
DEFAULT_C_PROGRAMS := $(patsubst $(SRC_PROG_DIR)/%.c,%,$(wildcard $(SRC_PROG_DIR)/*.c))
DEFAULT_PROGRAMS := $(sort $(DEFAULT_CPP_PROGRAMS) $(DEFAULT_C_PROGRAMS))
PROGRAMS ?= $(DEFAULT_PROGRAMS)
PROGRAM_OUTPUTS := $(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(PROGRAMS)))

ASFLAGS  := -f elf64
PROGRAM_CXXFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                    -m64 -mno-red-zone -mno-avx -mno-avx512f \
                    -fPIE -pie -Wall -Wextra -I. -I$(CRT_DIR) -I$(LIBC_DIR)/include -I../shared/include \
                    -I$(DEPENDENCY_INCLUDE_DIR)
PROGRAM_LDFLAGS := -Wl,--no-dynamic-linker
SHARED_CXXFLAGS := $(filter-out -fPIE -pie,$(PROGRAM_CXXFLAGS)) -fPIC
PROGRAM_CFLAGS := -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                  -m64 -mno-red-zone -mno-avx -mno-avx512f \
                  -fPIE -Wall -Wextra -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src \
                  -DBR_RDRAND=0 -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0 \
                  -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 \
                  -DBR_AES_X86NI=0 -DBR_SSE2=1 -DBR_POWER8=0
SHARED_CFLAGS := $(filter-out -fPIE,$(PROGRAM_CFLAGS)) -fPIC
SHARED_LDFLAGS := -shared -nostdlib

BEARSSL_SOURCE_DIR := $(BEARSSL_DIR)/src
BEARSSL_SOURCE_INCLUDE_DIR := $(BEARSSL_DIR)/inc
BEARSSL_SOURCES := $(if $(wildcard $(BEARSSL_SOURCE_DIR)),$(shell cd $(BEARSSL_SOURCE_DIR) && find . -type f -name '*.c' | sed 's#^\./##' | sort))
BEARSSL_OBJECTS := $(addprefix $(OBJ_DIR)/bearssl/,$(BEARSSL_SOURCES:.c=.o))
BEARSSL_SHARED_OBJECTS := $(addprefix $(OBJ_DIR)/bearssl-pic/,$(BEARSSL_SOURCES:.c=.o))
BEARSSL_LIB := $(OBJ_DIR)/libbearssl.a
BEARSSL_PIC_LIB := $(OBJ_DIR)/libbearssl-pic.a
SHARED_LIBC_OBJECTS := $(OBJ_DIR)/shared/libc_string.o
BEARSSL_SO := $(OUT_DIR)/libbearssl.so.0
BEARSSL_SO_LINK := $(OUT_DIR)/libbearssl.so
BEARSSL_STAGED_SO := $(LIB_DIR)/libbearssl.so.0
BEARSSL_STAGED_SO_LINK := $(LIB_DIR)/libbearssl.so
BEARSSL_STAGED_HEADERS := $(addprefix $(INCLUDE_DIR)/,$(notdir $(wildcard $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl*.h)))
BEARSSL_DEP_SO := $(DEPENDENCY_LIBRARY_DIR)/libbearssl.so.0
BEARSSL_DEP_HEADERS := $(if $(DEPENDENCY_ROOT),$(DEPENDENCY_INCLUDE_DIR)/bearssl.h,$(BEARSSL_STAGED_HEADERS))

# Opt programs into BearSSL with:
# PROGRAM_LIBS_<program> += $(BEARSSL_DEP_SO)
PROGRAM_DEPS_browse += $(BEARSSL_DEP_HEADERS)
PROGRAM_DEPS_download += $(BEARSSL_DEP_HEADERS)
PROGRAM_LIBS_browse += $(BEARSSL_PIC_LIB)
PROGRAM_LIBS_download += $(BEARSSL_PIC_LIB)
BEARSSL_PROGRAMS := browse download
SELECTED_BEARSSL_PROGRAMS := $(filter $(BEARSSL_PROGRAMS),$(PROGRAMS))
INSTALL_STAGED_LIBRARIES ?= 0
PROGRAM_HELPERS_browse += http
PROGRAM_HELPERS_installer += console
PROGRAM_HELPERS_insmod += args console
PROGRAM_HELPERS_lsdisk += console
PROGRAM_HELPERS_lsmod += console
PROGRAM_HELPERS_mount += args console
PROGRAM_HELPERS_mkneufs += args console
PROGRAM_HELPERS_mkpart += args console
PROGRAM_HELPERS_neupak += args text
PROGRAM_HELPERS_download += http
PROGRAM_HELPERS_netget += http
PROGRAM_DEPS_font += ../shared/include/TOSH-SAT.F14
PROGRAM_DEPS_desktop += ../shared/include/font8x8_basic.hpp desktop_protocol.hpp
PROGRAM_DEPS_networkd += net/ipv4.hpp net/network_protocol.hpp net/tcp.hpp net/udp.hpp
PROGRAM_DEPS_tcpd += net/network_protocol.hpp net/tcp.hpp net/tcpd_protocol.hpp net/udp.hpp

HDD_IMAGE ?= ../hdd.img
MOUNT_DIR ?= $(HOME)/MountedImage
MOUNT_UID ?= $(shell id -u)
MOUNT_GID ?= $(shell id -g)
MOUNT_OPTS ?= loop,rw,uid=$(MOUNT_UID),gid=$(MOUNT_GID),umask=022
RESET_USERS ?= 0

.PHONY: all bearssl bearssl-shared clean ffmpeg-minimal ffmpeg-configure \
        install install-reset-users install-mount install-mtools newlib-crt newlib-sdk
.SECONDEXPANSION:
.SECONDARY: $(addprefix $(NEWLIB_PROGRAM_OBJ_DIR)/,$(addsuffix .o,$(DEFAULT_C_PROGRAMS)))

ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif

all: $(PROGRAM_OUTPUTS)

$(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h:
	@echo "BearSSL not found at $(BEARSSL_DIR)." >&2
	@echo "Provide your own BearSSL checkout there if you want BearSSL available in userspace." >&2
	@false

$(INCLUDE_DIR)/bearssl.h: $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h
	@mkdir -p $(INCLUDE_DIR)
	@cp $< $@

$(INCLUDE_DIR)/bearssl%.h: $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl%.h
	@mkdir -p $(INCLUDE_DIR)
	@cp $< $@

$(OBJ_DIR)/crt0.o: $(CRT_DIR)/crt0.s $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(NASM) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/libc_string.o: $(LIBC_DIR)/string.cpp $(LIBC_DIR)/include/string.h | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/shared/libc_string.o: $(LIBC_DIR)/string.cpp $(LIBC_DIR)/include/string.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(SHARED_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/libc_ctype.o: $(LIBC_DIR)/ctype.cpp $(LIBC_DIR)/include/ctype.h | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/libc_neutrino.o: $(LIBC_DIR)/neutrino.cpp $(LIBC_DIR)/include/neutrino.h $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/helper_%.o: $(HELPER_DIR)/%.cpp $(HELPER_DIR)/%.hpp $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/bearssl/%.o: $(BEARSSL_SOURCE_DIR)/%.c $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(PROGRAM_CFLAGS) -c $< -o $@

$(OBJ_DIR)/bearssl-pic/%.o: $(BEARSSL_SOURCE_DIR)/%.c $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h $(BEARSSL_OBJECTS) | $(OBJ_DIR)
	$(AR) rcs $@ $(BEARSSL_OBJECTS)

# Programs link the archive directly so launching a TLS-capable utility does
# not require the kernel to stage an entire shared object in one contiguous
# buddy allocation.  The PIC objects are also valid in PIE executables.
$(BEARSSL_PIC_LIB): $(BEARSSL_SHARED_OBJECTS) | $(OBJ_DIR)
	$(AR) rcs $@ $(BEARSSL_SHARED_OBJECTS)

$(BEARSSL_SO): $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h $(BEARSSL_SHARED_OBJECTS) $(SHARED_LIBC_OBJECTS) | $(OUT_DIR)
	$(CC) $(SHARED_LDFLAGS) -Wl,-soname,libbearssl.so.0 $(BEARSSL_SHARED_OBJECTS) $(SHARED_LIBC_OBJECTS) -o $@

$(BEARSSL_SO_LINK): $(BEARSSL_SO)
	@cp $(BEARSSL_SO) $@

$(BEARSSL_STAGED_SO): $(BEARSSL_SO)
	@mkdir -p $(LIB_DIR)
	@cp $(BEARSSL_SO) $@

$(BEARSSL_STAGED_SO_LINK): $(BEARSSL_SO)
	@mkdir -p $(LIB_DIR)
	@cp $(BEARSSL_SO) $@

bearssl: $(BEARSSL_LIB)
bearssl-shared: $(BEARSSL_SO) $(BEARSSL_SO_LINK) $(BEARSSL_STAGED_SO) $(BEARSSL_STAGED_SO_LINK) $(BEARSSL_STAGED_HEADERS)

$(NEWLIB_SOURCE_DIR)/newlib/configure:
	@echo "Newlib not found at $(NEWLIB_SOURCE_DIR)." >&2
	@echo "Provide a Newlib checkout there before building the CRT." >&2
	@false

$(NEWLIB_CONFIG_STAMP): $(NEWLIB_SOURCE_DIR)/newlib/configure
	@mkdir -p $(NEWLIB_BUILD_DIR)
	cd $(NEWLIB_BUILD_DIR) && \
		$(abspath $(NEWLIB_SOURCE_DIR)/newlib/configure) \
			--host=$(NEWLIB_TARGET) \
			--prefix=/ \
			--disable-multilib \
			--disable-newlib-supplied-syscalls \
			--enable-newlib-multithread \
			--enable-newlib-retargetable-locking \
			--enable-newlib-io-c99-formats \
			--enable-newlib-io-long-long \
			--enable-newlib-register-fini \
			CC=$(NEWLIB_CC) \
			AR=$(NEWLIB_AR) \
			RANLIB=$(NEWLIB_RANLIB) \
			CFLAGS='-O2 -g -ffreestanding -fno-stack-protector -mno-red-zone -mno-avx -mno-avx512f -fPIE'
	@touch $@

$(NEWLIB_BUILD_DIR)/Makefile: $(NEWLIB_CONFIG_STAMP)
	@test -f $@

$(NEWLIB_LIBC) $(NEWLIB_LIBM) &: $(NEWLIB_BUILD_DIR)/Makefile
	$(MAKE) -C $(NEWLIB_BUILD_DIR) -j$(NEWLIB_JOBS)
	@mkdir -p $(NEWLIB_SYSROOT)
	$(MAKE) -C $(NEWLIB_BUILD_DIR) DESTDIR=$(abspath $(NEWLIB_SYSROOT)) install

$(NEWLIB_PORT_OBJ_DIR)/crt0.o: $(NEWLIB_PORT_DIR)/crt0.S | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/start.o: $(NEWLIB_PORT_DIR)/start.c $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/syscalls.o: $(NEWLIB_PORT_DIR)/syscalls.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/dirent.o: $(NEWLIB_PORT_DIR)/dirent.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_PORT_DIR)/include/dirent.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/pthread.o: $(NEWLIB_PORT_DIR)/pthread.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_PORT_DIR)/include/pthread.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/posix_compat.o: $(NEWLIB_PORT_DIR)/posix_compat.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_CRT0): $(NEWLIB_PORT_OBJ_DIR)/crt0.o $(NEWLIB_LIBC)
	@mkdir -p $(dir $@)
	cp $< $@

$(NEWLIB_LIBNEUTRINO): $(NEWLIB_PORT_OBJECTS) $(NEWLIB_LIBC)
	@mkdir -p $(dir $@)
	$(NEWLIB_AR) rcs $@ $(NEWLIB_PORT_OBJECTS)

$(NEWLIB_PLATFORM_INCLUDE_DIR)/%.h: $(NEWLIB_PORT_DIR)/include/%.h $(NEWLIB_LIBC)
	@mkdir -p $(dir $@)
	cp $< $@

$(NEWLIB_PORT_OBJ_DIR):
	mkdir -p $@

$(NEWLIB_PROGRAM_OBJ_DIR):
	mkdir -p $@

newlib-crt: $(NEWLIB_CRT0) $(NEWLIB_LIBNEUTRINO) $(NEWLIB_PLATFORM_HEADERS) $(NEWLIB_LIBC) $(NEWLIB_LIBM)
newlib-sdk: newlib-crt

ffmpeg-configure: newlib-sdk
	bash $(NEWLIB_PORT_DIR)/configure-ffmpeg.sh \
		$(FFMPEG_SOURCE_DIR) $(FFMPEG_BUILD_DIR) $(FFMPEG_THREAD_MODE)

ffmpeg-minimal: ffmpeg-configure
	$(MAKE) -C $(FFMPEG_BUILD_DIR) -j$(NEWLIB_JOBS) ffmpeg

INSTALL_PREREQS := $(if $(filter clean,$(MAKECMDGOALS)),clean) all
program_helper_objects = $(addprefix $(OBJ_DIR)/helper_,$(addsuffix .o,$(PROGRAM_HELPERS_$(1))))

$(NEWLIB_PROGRAM_OBJ_DIR)/%.o: $(SRC_PROG_DIR)/%.c $(NEWLIB_CRT0) $(NEWLIB_LIBNEUTRINO) $(NEWLIB_PLATFORM_HEADERS) | $(NEWLIB_PROGRAM_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PROGRAM_CFLAGS) -c $< -o $@

$(OUT_DIR)/%.elf: $(SRC_PROG_DIR)/%.c $(NEWLIB_PROGRAM_OBJ_DIR)/%.o $(NEWLIB_CRT0) $(NEWLIB_LIBNEUTRINO) $(NEWLIB_PLATFORM_HEADERS) $(NEWLIB_LIBC) $(NEWLIB_LIBM) Makefile | $(OUT_DIR)
	@mkdir -p $(OUT_DIR) $(BIN_DIR)
	$(NEWLIB_CC) $(NEWLIB_CFLAGS) -pie \
		-Wl,--no-dynamic-linker,-z,noexecstack \
		$(NEWLIB_CRT0) \
		$(NEWLIB_PROGRAM_OBJ_DIR)/$*.o \
		$(NEWLIB_LINK_LIBS) \
		-o $@
	@cp $@ $(BIN_DIR)/$*.elf

$(OUT_DIR)/%.elf: $(SRC_PROG_DIR)/%.cpp $(OBJ_DIR)/crt0.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_neutrino.o $(CRT_DIR)/syscall.hpp $$(PROGRAM_DEPS_$$*) $$(PROGRAM_LIBS_$$*) $$(call program_helper_objects,$$*) | $(OUT_DIR)
	@mkdir -p $(OUT_DIR) $(BIN_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) $(PROGRAM_LDFLAGS) $(OBJ_DIR)/crt0.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_neutrino.o $(call program_helper_objects,$*) $< $(PROGRAM_LIBS_$*) -o $@
	@cp $@ $(BIN_DIR)/$*.elf

install: $(INSTALL_PREREQS)
	@set -euo pipefail; \
	mkdir -p "$(MOUNT_DIR)"; \
	mounted=0; \
	cleanup() { if [[ $$mounted -eq 1 ]]; then $(MOUNT_SUDO) umount "$(MOUNT_DIR)"; fi; }; \
	trap cleanup EXIT INT TERM; \
	$(MOUNT_SUDO) mount -o "$(MOUNT_OPTS)" "$(HDD_IMAGE)" "$(MOUNT_DIR)"; \
	mounted=1; \
	$(SUDO) rm -rf "$(MOUNT_DIR)/binary"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/binary"; \
	$(SUDO) rm -rf "$(MOUNT_DIR)/library"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/library"; \
	$(SUDO) rm -rf "$(MOUNT_DIR)/config"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/config"; \
	if [[ "$(RESET_USERS)" == "1" ]]; then \
		$(SUDO) rm -f "$(MOUNT_DIR)/system/users.ntd" "$(MOUNT_DIR)/users.ntd"; \
	fi; \
	for prog in $(PROGRAMS); do \
		$(SUDO) install -m 0644 "$(OUT_DIR)/$$prog.elf" "$(MOUNT_DIR)/binary/$$prog.elf"; \
	done; \
	if [[ "$(INSTALL_STAGED_LIBRARIES)" == "1" && -d "$(LIB_DIR)" ]]; then \
		for lib in "$(LIB_DIR)"/*; do \
			if [[ -f "$$lib" ]]; then \
				$(SUDO) install -m 0644 "$$lib" "$(MOUNT_DIR)/library/$$(basename "$$lib")"; \
			fi; \
		done; \
	fi; \
	if [[ -d "$(CONFIG_DIR)" ]]; then \
		$(SUDO) cp -a "$(CONFIG_DIR)/." "$(MOUNT_DIR)/config/"; \
	fi; \
	if [[ -f "$(CONFIG_DIR)/KERNEL.CFG" ]]; then \
		$(SUDO) install -m 0644 "$(CONFIG_DIR)/KERNEL.CFG" "$(MOUNT_DIR)/KERNEL.CFG"; \
	fi; \
	if [[ -f "$(OUT_DIR)/init.elf" ]]; then \
		$(SUDO) install -m 0644 "$(OUT_DIR)/init.elf" "$(MOUNT_DIR)/init.elf"; \
	fi; \
	sync

install-reset-users:
	$(MAKE) install RESET_USERS=1

install-mtools: $(INSTALL_PREREQS)
	@set -euo pipefail; \
	mmd -i "$(HDD_IMAGE)" ::/binary ::/library ::/config; \
	for prog in $(PROGRAMS); do \
		mcopy -i "$(HDD_IMAGE)" "$(OUT_DIR)/$$prog.elf" "::/binary/$$prog.elf"; \
	done; \
	if [[ "$(INSTALL_STAGED_LIBRARIES)" == "1" && -d "$(LIB_DIR)" ]]; then \
		for lib in "$(LIB_DIR)"/*; do \
			if [[ -f "$$lib" ]]; then \
				mcopy -i "$(HDD_IMAGE)" "$$lib" "::/library/$$(basename "$$lib")"; \
			fi; \
		done; \
	fi; \
	if [[ -d "$(CONFIG_DIR)" ]]; then \
		mcopy -s -i "$(HDD_IMAGE)" "$(CONFIG_DIR)"/* ::/config/; \
	fi; \
	if [[ -f "$(CONFIG_DIR)/KERNEL.CFG" ]]; then \
		mcopy -i "$(HDD_IMAGE)" "$(CONFIG_DIR)/KERNEL.CFG" ::/KERNEL.CFG; \
	fi; \
	if [[ -f "$(OUT_DIR)/init.elf" ]]; then \
		mcopy -i "$(HDD_IMAGE)" "$(OUT_DIR)/init.elf" ::/init.elf; \
	fi

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OBJ_DIR) $(OUT_DIR) $(BIN_DIR) $(LIB_DIR) $(INCLUDE_DIR)
