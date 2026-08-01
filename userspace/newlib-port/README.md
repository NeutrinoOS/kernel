# Neutrino Newlib port

This directory is the tracked Neutrino platform layer for Newlib. The upstream
checkout remains unmodified under `userspace/deps/newlib-cygwin`.

Run `make -C userspace newlib-sdk` to configure and build a static, single-
threaded Newlib plus the Neutrino startup and syscall objects. The resulting
sysroot contains `crt0.o`, `libneutrino.a`, `libc.a`, `libm.a`, and the
Neutrino platform headers under
`userspace/build/newlib-pie-sysroot`. Platform headers are kept in a separate
overlay include directory so rebuilding upstream Newlib cannot overwrite them.

For small C programs, `userspace/newlib-port/neutrino-cc` is a compiler driver
that supplies the required freestanding, PIE, startup, sysroot, and static
library arguments:

```sh
userspace/newlib-port/neutrino-cc program.c -o program.elf
```

Set `NEUTRINO_SYSROOT`, `NEUTRINO_TARGET`, or `NEUTRINO_CROSS_CC` to consume a
different built sysroot or cross compiler. The driver also supports ordinary
compile-only invocations such as `-c` and exposes its target sysroot through
`--print-sysroot`.

Production C programs under `userspace/programs` are built through this SDK and
installed under their normal command names. For example, `ls.c` uses
`opendir`, `readdir`, `printf`, and `closedir` without including the raw
Neutrino syscall interface:

```sh
make -C userspace PROGRAMS="ls cat wc date sleep" all
```

The first milestone supports:

- C startup, initialization/finalization arrays, `exit`, `argc`, and `argv`
- standard input, output, and error descriptors
- sequential file reads and writes
- 64-bit seek, file/path metadata, positioned reads, and append/create flags
- POSIX-style directory iteration with `opendir`, `readdir`, `closedir`,
  `rewinddir`, `telldir`, and `seekdir`
- Newlib allocation over a bounded, demand-paged anonymous mapping
- wall-clock time, process identity, and entropy
- basic filesystem mutation, synchronization, and current-directory calls
- `nanosleep`, `sleep`, and `usleep`
- monotonic/realtime clocks, anonymous/private-file mappings, and page protection
- pthread threads, mutexes, condition variables, once controls, and keys

Current ABI limitations:

- Neutrino passes a command argument string but not the executable path, so
  `argv[0]` is currently an empty string.
- Kernel failures are not structured error numbers, so several failures map to
  conservative `errno` values.
- `O_TRUNC`, file-length changes, timed descriptor polling, and full terminal
  control remain unsupported.
- Compiler-generated ELF TLS is not loaded yet. Newlib reentrancy and pthread
  state use the kernel thread ID and explicit thread control records instead.

## Migration direction

Ordinary command-line programs should prefer libc interfaces such as `stdio`,
`unistd`, `dirent`, and `time`; the converted production commands demonstrate
that layer. Raw syscalls remain private to `libneutrino.a`.

Hardware devices, graphical sessions, capability transfer, and other
Neutrino-specific facilities should eventually move behind focused public
headers under a `neutrino/` namespace. They should not be disguised as POSIX
interfaces where the semantics do not match. Existing programs can therefore
migrate incrementally: libc first for ordinary runtime services, then the
smaller Neutrino API only where an OS-specific feature is actually required.

## Minimal FFmpeg build

The build automatically uses the sibling checkout at
`../neutrino-packages/ffmpeg/src` when present, falling back to
`userspace/deps/ffmpeg`. Then run:

```sh
make -C userspace ffmpeg-minimal
```

This configures a static CLI with the file protocol, Matroska/WebM demuxing,
VP8/VP9 decoding, and raw/null output. Set `FFMPEG_THREAD_MODE=threadless` for
the first single-thread bring-up; the default uses pthread frame workers. The
result is `userspace/build/ffmpeg-minimal/ffmpeg`.
