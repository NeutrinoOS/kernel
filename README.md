# neutrino
Neutrino is a novel, x86_64 kernel written in C++. More information to come

## Building
All of these methods will likely require some changes to variables defined at the top of the file, since some arch packages (ovmf firmware and cross-compilers) use somewhat nonstandard names and paths, as far as I'm aware. You'll want to have a C++20 conforming compiler that can output x86_64 ELF files (preferably GCC, and its accompanying binutils), git, xorriso, nasm, and make. You'll also want qemu-system-x86_64 and optionally OVMF firmware for make run and make debug targets.
### ISO
To get an ISO that runs on 64-bit machines via legacy or UEFI boot, run `make iso`. An iso file will be produced in out/neutrino.iso by default.
### QEMU
To build the ISO and automatically run it in QEMU with OVMF firmware, run `make run`. This might require some tweaking as my machine uses Arch, which has a nonstandard location for OVMF firmware. It can also be run manually without providing an OVMF path to use legacy boot.
### Raw .elf
To get only the raw kernel.elf file, you can run `make all`. This will produce out/kernel.elf, which is not very useful on its own, but I don't know your motivations.

Installable kernel updates are packaged from the sibling
`neutrino-packages/neutrino-kernel` project. Run `make package` there after
bumping its semantic version. The package build embeds that manifest version in
the kernel, and the running version is reported by `uname -r`. Direct builds use
`0.0.0-dev` unless `KERNEL_VERSION` is supplied. Boot packages target the
installed Neutrino EFI system partition and cannot be uninstalled; install a
known-good replacement to roll back. Before replacement, neupak preserves the
running kernel as
`/boot/kernel.previous.elf`; new installations expose that copy as a recovery
entry in the Limine boot menu.

The live image also carries a complete, versioned userspace package set in
`/packages`. Every live userspace file is preinstalled from the standalone
projects in the sibling `neutrino-packages` repository; the main repository
only retains the userspace SDK and platform headers. The bundled repository has
a normal Neupak index and a `file:///packages/index.toml` source, so the NEUFS
installer can use dependency resolution while completely offline. It installs
the `neutrino-live` package set below `--root <mounted-root>` instead of cloning
the live filesystem. Package archives are generated in each package project's
`out` directory.
### Debugging
You can use `make debug` to run in EFI mode with QEMU's debug mode enabled, allowing you to attach a debugger with e.g. `gdb -ex "target remote localhost:1234" -ex "symbol-file out/kernel.elf"`. The same considerations apply as with normal `make run.`
### Optimized builds
There's no support right now for optimized builds out of the box, but you can run something like `make clean all EXTRA_CFLAGS="-O3 -DNDEBUG=1"` to pass -O3 -DNDEBUG=1 into CFLAGs.

## Userspace SDK and packages

Run `make -C userspace newlib-sdk` to build the Newlib sysroot used by C-based
packages. Runnable programs, third-party libraries such as BearSSL, package
manifests, and root configuration live in the sibling `neutrino-packages`
repository. `make live-rootfs` builds the selected package set and assembles the
live filesystem exclusively from those archives.

The default live package roots are `neutrino-installer` and `neutrino-live`.
Their manifests expand to the complete live environment. Override
`LIVE_PACKAGES` to stage a custom set into `make live-rootfs`, `make iso`, or
`make run`; manifest dependencies are included automatically and ordered before
the packages that require them. For example:

```sh
make print-live-packages LIVE_PACKAGES="network-tools editor-tools"
make iso LIVE_PACKAGES="network-tools editor-tools"
make run LIVE_PACKAGES="network-tools editor-tools"
```

`print-live-packages` previews the dependency-expanded package order without
building the image. A custom set only contains the requested packages and their
dependencies, so include `neutrino-installer` when the resulting live system
should provide the installer.

## Virtual memory

Neutrino tracks executable images, private loader allocations, guarded user
stacks, anonymous mappings, shared memory, framebuffer/device mappings, and
private file mappings as per-address-space VM areas.

Anonymous mappings are demand-zero: physical pages are allocated only when
userspace first accesses them or a syscall copies through them. Private file
mappings use syscall ABI 1.5 and `map_file_private()`. File offsets must be
page-aligned. Read-only pages can be shared through the bounded kernel page
cache; writable private mappings copy an individual page on its first write.
The current cache holds at most 256 pages and file mappings larger than that
limit fail cleanly rather than falling back to unbounded allocation.

`top` reports resident memory in KiB from the same VM-area accounting used by
the kernel.

## Threads and process control

Syscall ABI 1.6 added native execution threads. `thread_create()` starts an
independently scheduled context with its own guarded user stack, kernel stack,
register/FPU state, and thread ID while sharing its process address space.
Threads can terminate independently with `thread_exit()` and remain joinable
until `thread_join()` collects their exit status. `thread_id()` and
`process_id()` expose the two identity levels, and `top` distinguishes threads
from process leaders.

`futex_wait()` and `futex_wake()` provide process-private blocking on aligned
32-bit userspace words, allowing uncontended synchronization to stay entirely
in userspace. Process exit terminates the complete thread group and address
spaces remain alive until their final thread is reclaimed.

The process model has two independently reference-counted ownership
objects. Threads share both their address space and a process-resource object
containing descriptors, file and directory handles, capabilities, console
identity, and current directory. Table operations, shared file offsets, CWD
updates, and credential changes are serialized across the thread group; the
resources are closed only after its final thread is reclaimed.

Syscall ABI 1.7 completes the native process-control layer. It adds per-thread
TLS base management and TLS-aware thread creation; bounded asynchronous event
queues; process-group kill, suspend, and resume; arbitrary-child waiting with
zombie collection; process groups, sessions, and foreground terminal groups;
enforced thread, descriptor, handle, address-space, and CPU-time limits with
usage accounting; and a capability-gated tracer that can stop a process,
inspect or modify its memory, and read a stopped thread's registers. External
control requires the `ProcessControl` capability, while cross-process
observation continues to require `Monitor`.

## Debug heartbeat

Add the standalone `DEBUG` token to the kernel command line to enable a 3x3
color-changing scheduler heartbeat in the top-right corner of the framebuffer.
The indicator is disabled by default.

## ACPI diagnostic flags

The following standalone kernel command-line tokens stop uACPI at progressively
later initialization checkpoints. Early table access remains enabled so MADT
discovery and IOAPIC routing can still work.

- `ACPI=OFF` skips full uACPI runtime initialization.
- `ACPI.NO_MODE` prevents uACPI from switching the firmware into ACPI mode.
- `ACPI.NO_NAMESPACE_LOAD` initializes the uACPI core but does not load AML.
- `ACPI.NO_NAMESPACE_INIT` loads AML but does not run namespace `_REG`, `_STA`,
  or `_INI` initialization.

The flags can be combined, such as `ACPI.NO_MODE ACPI.NO_NAMESPACE_INIT`, to
keep firmware mode unchanged while loading the namespace without executing its
initialization methods. The selected checkpoints are written to the kernel log.

## Intel UHD diagnostic flag

Add `INTEL_UHD=OFF` to the kernel command line to prevent the Intel UHD driver
from binding and adopting the firmware-configured scanout. Neutrino continues
using the framebuffer supplied by Limine. The alternate spelling
`INTEL_UHD.DISABLE` is also accepted.

## Desktop

The `desktop` program is a small compositor and window manager. It owns the
graphical session, composites application-owned ARGB surfaces through the
desktop pipe protocol, and presents only the framebuffer rectangle damaged by
an application, cursor movement, or window movement.

Appearance and the taskbar launcher are configured in
`/config/desktop.cfg`. Click a window to focus it, drag its title bar to move
it, drag its bottom-right grip to resize it, and use its red title-bar button
to request that it close. The launcher button starts its configured
application; F12 exits the desktop. The `doomgeneric` package supports the
protocol and retains fullscreen fallback when no desktop is running.

## Live network diagnostics

The live ISO includes several tools for separating driver, DHCP, routing, DNS,
and TCP failures:

- `netctl info 0` shows link state, MAC address, MTU, IPv4 mode, address,
  netmask, gateway, and DNS server for interface 0.
- `netctl debug 0` shows the NIC's ring/register snapshot and packet counters.
- `netctl status` prints a layered health assessment (device, carrier/cable,
  IPv4, gateway, DNS, NIC rings, ARP, DHCP, networkd, and tcpd) followed by
  detailed traffic, rejection, timeout, and protocol counters.
- `ping <ipv4>` tests ARP, routing, and ICMP without depending on DNS or TCP.
- `netget <host-or-url>` tests DNS and an unencrypted HTTP connection through
  tcpd. Use `download` when testing HTTPS.
- `browse <url>` exercises the interactive HTTP/HTTPS path, including DNS, TCP,
  TLS certificate validation, and the live image's CA trust store.
- `lspci` confirms the detected Ethernet controller and its PCI identity.

## ISA hardware-monitor sensors

Neutrino probes the standard Super-I/O configuration ports for supported ITE
IT87-family environment controllers. The probe is read-only: it accepts only a
known chip ID and an already-enabled, aligned hardware-monitor I/O range.

Run `sensors` to list the temperature and voltage channels reported by a
detected adapter. Unsupported or disabled controllers are left unchanged and
do not appear in the output.

ACPI thermal-zone objects are also registered after the firmware namespace is
fully initialized. Their `_TMP` readings appear as separate ACPI adapters in
the same `sensors` output. Diagnostic modes that skip ACPI namespace
initialization intentionally skip thermal-zone discovery as well.
