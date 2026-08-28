# Neutrino userspace SDK build helpers.
#
# Runnable userspace programs are standalone packages in ../neutrino-packages.
# This Makefile only builds the Newlib-based SDK retained by the main tree.

SHELL := /bin/bash

OBJ_DIR := build
DEPS_DIR := deps
NEWLIB_SOURCE_DIR ?= $(DEPS_DIR)/newlib-cygwin
NEWLIB_PORT_DIR := newlib-port
NEWLIB_BUILD_DIR := $(OBJ_DIR)/newlib-pie-build
NEWLIB_CONFIG_STAMP := $(NEWLIB_BUILD_DIR)/.neutrino-multithread-configured
NEWLIB_SYSROOT := $(OBJ_DIR)/newlib-pie-sysroot
NEWLIB_TARGET := x86_64-elf
NEWLIB_CC ?= $(NEWLIB_TARGET)-gcc
NEWLIB_CXX ?= $(NEWLIB_TARGET)-g++
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
NEWLIB_PLATFORM_HEADER_NAMES := arpa/inet.h dlfcn.h dirent.h netdb.h netinet/in.h netinet/tcp.h \
                               poll.h pthread.h sched.h stdatomic.h sys/ioctl.h \
                               sys/mman.h sys/random.h sys/socket.h sys/utsname.h \
                               time.h unistd.h
NEWLIB_PLATFORM_HEADERS := $(addprefix $(NEWLIB_PLATFORM_INCLUDE_DIR)/,$(NEWLIB_PLATFORM_HEADER_NAMES))
NEWLIB_PORT_OBJ_DIR := $(OBJ_DIR)/newlib-port
NEWLIB_PORT_OBJECTS := $(NEWLIB_PORT_OBJ_DIR)/start.o \
                       $(NEWLIB_PORT_OBJ_DIR)/syscalls.o \
                       $(NEWLIB_PORT_OBJ_DIR)/dirent.o \
                       $(NEWLIB_PORT_OBJ_DIR)/pthread.o \
                       $(NEWLIB_PORT_OBJ_DIR)/posix_compat.o \
                       $(NEWLIB_PORT_OBJ_DIR)/dlfcn.o \
                       $(NEWLIB_PORT_OBJ_DIR)/socket.o \
                       $(NEWLIB_PORT_OBJ_DIR)/libc_optimizations.o \
                       $(NEWLIB_PORT_OBJ_DIR)/malloc_locks.o
NEWLIB_CFLAGS := -O2 -g -ffreestanding -fno-builtin \
                 -fno-stack-protector -nostdlib -m64 -mno-red-zone \
                 -mno-avx -mno-avx512f -fPIE -D_POSIX_THREADS=200809L
NEWLIB_PORT_CFLAGS := $(NEWLIB_CFLAGS) -std=c11 -Wall -Wextra -Wpedantic \
                      -I$(NEWLIB_PORT_DIR)/include \
                      -isystem $(NEWLIB_INCLUDE_DIR) -I$(NEWLIB_PORT_DIR)
NEWLIB_PORT_CXXFLAGS := $(filter-out -std=c11,$(NEWLIB_PORT_CFLAGS)) -std=c++17 \
                        -fno-gnu-unique \
                        -I../shared/include \
                        -I../../neutrino-packages/userspace-support
FFMPEG_SIBLING_SOURCE_DIR := $(abspath ../../neutrino-packages/ffmpeg/src)
FFMPEG_SOURCE_DIR ?= $(if $(wildcard $(FFMPEG_SIBLING_SOURCE_DIR)/configure),$(FFMPEG_SIBLING_SOURCE_DIR),$(DEPS_DIR)/ffmpeg)
FFMPEG_BUILD_DIR ?= $(OBJ_DIR)/ffmpeg-minimal
FFMPEG_THREAD_MODE ?= pthread

.PHONY: all clean newlib-crt newlib-sdk ffmpeg-configure ffmpeg-minimal

all: newlib-sdk

$(NEWLIB_SOURCE_DIR)/newlib/configure:
	@echo "Newlib not found at $(NEWLIB_SOURCE_DIR)." >&2
	@echo "Provide a Newlib checkout there before building the SDK." >&2
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
			CC=$(NEWLIB_CC) AR=$(NEWLIB_AR) RANLIB=$(NEWLIB_RANLIB) \
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

$(NEWLIB_PORT_OBJ_DIR)/syscalls.o: $(NEWLIB_PORT_DIR)/syscalls.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_PORT_DIR)/include/time.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/dirent.o: $(NEWLIB_PORT_DIR)/dirent.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_PORT_DIR)/include/dirent.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/pthread.o: $(NEWLIB_PORT_DIR)/pthread.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_PORT_DIR)/include/pthread.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/posix_compat.o: $(NEWLIB_PORT_DIR)/posix_compat.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_PORT_DIR)/include/sys/utsname.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/dlfcn.o: $(NEWLIB_PORT_DIR)/dlfcn.c $(NEWLIB_PORT_DIR)/neutrino_syscall.h $(NEWLIB_PORT_DIR)/include/dlfcn.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/socket.o: $(NEWLIB_PORT_DIR)/socket.cpp $(NEWLIB_PORT_DIR)/socket_internal.h $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CXX) $(NEWLIB_PORT_CXXFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/libc_optimizations.o: $(NEWLIB_PORT_DIR)/libc_optimizations.c $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
	$(NEWLIB_CC) $(NEWLIB_PORT_CFLAGS) -c $< -o $@

$(NEWLIB_PORT_OBJ_DIR)/malloc_locks.o: $(NEWLIB_PORT_DIR)/malloc_locks.c $(NEWLIB_LIBC) | $(NEWLIB_PORT_OBJ_DIR)
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

newlib-crt: $(NEWLIB_CRT0) $(NEWLIB_LIBNEUTRINO) $(NEWLIB_PLATFORM_HEADERS) $(NEWLIB_LIBC) $(NEWLIB_LIBM)
newlib-sdk: newlib-crt

ffmpeg-configure: newlib-sdk
	bash $(NEWLIB_PORT_DIR)/configure-ffmpeg.sh $(FFMPEG_SOURCE_DIR) $(FFMPEG_BUILD_DIR) $(FFMPEG_THREAD_MODE)

ffmpeg-minimal: ffmpeg-configure
	$(MAKE) -C $(FFMPEG_BUILD_DIR) -j$(NEWLIB_JOBS) ffmpeg

clean:
	rm -rf $(OBJ_DIR)
