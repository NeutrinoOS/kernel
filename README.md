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

### Userspace core dumps

Add `COREDUMP=ON` (or the bare `COREDUMP` token) to the kernel command line to
write an ELF64 core file when a userspace task terminates because of a CPU
exception such as #DE, #UD, #GP, or an unrecoverable #PF. Dumps are written to
`/cores/core.<pid>.<tid>.<sequence>`, exclude shared and device mappings, and
include at most 64 MiB of private process memory. When the root filesystem
supports ACLs, the dump is restricted to the user that owned the crashed task;
if that protection cannot be applied, the incomplete dump is removed.

Load the executable and dump in GDB with, for example,
`gdb /path/to/program /cores/core.12.12.1`. Core dumping is disabled by default
because dumps contain private memory and consume persistent storage.
### Optimized builds
There's no support right now for optimized builds out of the box, but you can run something like `make clean all EXTRA_CFLAGS="-O3 -DNDEBUG=1"` to pass -O3 -DNDEBUG=1 into CFLAGs.

## Userspace SDK and packages

Run `make -C userspace newlib-sdk` to build the Newlib sysroot used by C-based
packages. Runnable programs, third-party libraries such as BearSSL, package
manifests, and root configuration live in the sibling `neutrino-packages`
repository. `make live-rootfs` builds the selected package set and assembles the
live filesystem exclusively from those archives.

The default live package roots are `neutrino-installer`, `neutrino-live`, and
the `neutrino-drivers` metapackage.
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

## Loadable hardware drivers

Hardware that is not common to every PC is shipped outside the kernel as
loadable modules. The live image includes the `e1000e`, `virtio-net`,
`intel-hda`, and `intel-uhd-gemini-lake` packages through the
`neutrino-drivers` metapackage. At boot, the module loader reads each module's
PCI match table and keeps only drivers that apply to detected hardware.

Each standalone driver package owns its implementation, private headers, and
`.ko` build. The Neutrino source tree contains only the shared module loader and
kernel-side provider interfaces; it does not retain copies of those hardware
driver sources.

The NEUFS installer queries that loaded-module set and installs only the
matching driver packages on the target. It also writes the installed system's
`/modules/loads.txt` from that set. The legacy FAT32 clone path removes
nonmatching driver packages after cloning and generates the same tailored load
list.

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

On x86-64 systems with PCID support, each process address space receives a
hardware context identifier. Threads inherit the identifier with their shared
address space, allowing context switches to retain tagged TLB entries. Page
table changes and identifier reuse use targeted SMP shootdowns; CPUs without
PCID support continue to use ordinary flushing CR3 switches.

Syscall ABI 1.7 completes the native process-control layer. It adds per-thread
TLS base management and TLS-aware thread creation; bounded asynchronous event
queues; process-group kill, suspend, and resume; arbitrary-child waiting with
zombie collection; process groups, sessions, and foreground terminal groups;
enforced thread, descriptor, handle, address-space, and CPU-time limits with
usage accounting; and a capability-gated tracer that can stop a process,
inspect or modify its memory, and read a stopped thread's registers. External
control requires the `ProcessControl` capability, while cross-process
observation requires `ProcessInspect` and tracing requires the separate
`ProcessTrace` capability.

## Capabilities and filesystem permissions

Capabilities authorize privileged kernel operations; they are not file or
disk ownership permissions. NeuFS reads, writes, creation, deletion, and ACL
updates are authorized by NeuFS ACLs. Filesystems without ACL support retain
their filesystem-specific behavior. `FilesystemOverride` is the explicit
administrative ACL bypass and is intentionally separate from `IdentityManage`.

The current capability bits are:

This layout intentionally replaces the earlier coarse mask; persisted masks
from pre-4.0 kernels must be recreated rather than reinterpreted.

| Bit | Capability | Authority |
| ---: | --- | --- |
| 0 | `SystemWriteSettings` | Read and change global console and kernel settings |
| 1 | `SystemPower` | Shutdown and reset |
| 2 | `FilesystemMount` | Mount a filesystem |
| 3–5 | `StorageRawRead`, `StorageRawWrite`, `StorageManage` | Raw media I/O and global storage management |
| 6–9 | `ProcessSpawn`, `ProcessInspect`, `ProcessControl`, `ProcessTrace` | Process lifecycle, observation, control, and debugging |
| 10–11 | `IdentityManage`, `ModuleLoad` | Users/principals and kernel modules |
| 12–14 | `GraphicalSession`, `InputDevices`, `Audio` | Desktop display, input-seat, and audio access |
| 15–16 | `Network`, `NetworkManage` | Network endpoints and network-device administration |
| 17–18 | `Serial`, `Pci` | Direct serial and PCI access |
| 19–20 | `SystemMonitor`, `KernelLog` | System telemetry and kernel logs |
| 21 | `FilesystemOverride` | Administrative ACL bypass |
| 22 | `SystemReadSettings` | Read global console and kernel settings |

Pipes, shared memory, access-controlled VTYs, and the service registry are
ordinary process/session primitives and require no capability. Raw storage
capabilities govern direct device descriptors only; mounted-file access remains
a filesystem permission decision.

## Replaceable userspace services

Named userspace daemons such as `networkd`, `tcpd`, and `dhcp` are default
providers. Applications look up a stable service identifier and ABI
version through the kernel service registry:

```text
net.neutrino.network   ABI 1
net.neutrino.tcp       ABI 1
net.neutrino.dhcp      ABI 1
```

The `libnet` package installs `libnet.so.0` plus `include/neutrino/net.hpp` and
`include/neutrino/http.hpp`. That one library is the userspace interface to the
Network/TCP ABIs and to HTTP client and server protocol machinery. Replacing a
provider does not require rebuilding applications; existing connections are
not migrated.

Syscall ABI 2.0 renumbers the complete syscall table into contiguous subsystem
groups. ABI discovery remains at calls 0 and 1; core system services,
descriptors, filesystem operations, virtual memory, threads, processes,
security, and privileged administration follow in that order. All userspace
must be rebuilt against the ABI 2.0 headers because the remaining numeric call
identifiers are intentionally incompatible with ABI 1.x.

ABI 2.2 gives every authenticated userspace principal a private persistent
settings hive. `settings_get` and `settings_set` always address that caller's
hive; no API accepts another user identity. Its file ACL has exactly one entry
for the owning user, and ACL-less filesystems fail closed. Kernel callers keep
the machine registry as their default. Userspace accesses it only through
`machine_settings_get` (requiring `SystemReadSettings` or
`SystemWriteSettings`) and `machine_settings_set` (requiring
`SystemWriteSettings`); write authority inherently grants reads. Both hives
also provide index-based key enumeration: `settings_key_at` and
`machine_settings_key_at` return the NUL-inclusive key length and use `-1` as
the end-of-list/error result, so callers do not need a racy count query.

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

`INTEL_UHD.RCS_PROBE` is an opt-in render-engine diagnostic for supported
Gemini Lake devices (`0x3184` and `0x3185`). It enables Gen9 execlist
submission, builds a kernel-owned 22-page logical render context with a
private PPGTT, maps a batch at GPU VA `0x00100000`, and submits it twice through
Gen8 `MI_BATCH_BUFFER_START` using the non-secure PPGTT selector. Each batch
writes an i915-style RCS `PIPE_CONTROL` breadcrumb and terminates with
`MI_BATCH_BUFFER_END`; the second submission reuses the GPU-saved context with
an advanced logical-ring tail. The batch page and an alias of the kernel
completion page occupy consecutive private PPGTT pages, so the non-secure
batch never writes through the global GGTT selector.
Before its breadcrumb, the batch performs the required two-phase cache flush
and selects the Gen9 3D pipeline with masked `PIPELINE_SELECT` fields. It then
programs Mesa-compatible AA-line, drawing-rectangle, and WM-chromakey defaults
to validate fixed-function `3DSTATE` command parsing. A dedicated private
PPGTT page at `0x00102000` backs the general, surface, dynamic, indirect,
instruction, and bindless heaps installed by Gen9 `STATE_BASE_ADDRESS`.
BCS ring programming and submission use Gemini Lake's separate Gen9 blitter
force-wake request and ACK registers. If a BCS request still times out, the
driver disables that accelerator for the remainder of the boot so compositor
damage immediately uses the kernel's CPU presentation fallback instead of
repeatedly stalling and resetting the ring.
The state heap also contains a PS binding table, a linear 64x64
`B8G8R8A8_UNORM` render-target surface backed by four private PPGTT pages, and
a Mesa-generated Gen9 constant-color fragment kernel. The batch programs the
matching SIMD8/SIMD16 `3DSTATE_PS`, `PS_EXTRA`, and blend state, configures the
Gen9 URB and fixed-function vertex-fetch stages, and issues a three-vertex
`RECTLIST` primitive using Mesa's no-VS BLORP path. Its vertices live in one
additional kernel-owned private PPGTT page. After both requests retire and the
render cache is flushed, the driver verifies that the expected orange pixels
were written to the render target. Success is logged as
`Gen9 render-target write probe completed submissions=2 pixels=<count>`. It
does not expose command submission to userspace or change the normal
BLT/display path, so it remains a bounded hardware bring-up test before
Mesa-facing context support is added.

On failure the driver also prints a grouped `RCSDBG1` token containing the
same RCS register snapshot and a transcription-checking CRC. Only that token
needs to be copied for debugging; it can be decoded with
`intel-uhd-gemini-lake/decode_rcs_debug.py` in the packages repository.

libdrm exposes render ABI 1.5 device discovery through
`neutrino_render_get_device_info()`. The query reports the PCI identity,
graphics generation, engine mask, BO limits, and runtime-validated execlist,
PPGTT32, 3D-pipeline, state-base, fragment-shader, and render-target-write
capabilities. The fragment and render-target bits are published only after the
diagnostic draw's pixels pass CPU verification. A separate bounded-demo bit
advertises the kernel-owned 64x64 userspace demo request. Explicit cache-domain
transitions for mapped BO ranges are available through
`neutrino_render_sync_bo()`, giving a future Mesa winsys a defined CPU-to-GPU
and GPU-to-CPU coherency boundary. Synchronization is rejected while work is
in flight for the context. Fence waits poll the timeline from userspace and
sleep between checks, ensuring a caller on CPU 0 cannot starve deferred GPU
completion. The general
`NEUTRINO_RENDER_CAP_USER_SUBMISSION` bit remains clear until a validated
programmable command-stream interface suitable for Mesa is available;
hardware discovery and bounded semantic requests never authorize raw command
submission.

The optional `intel-uhd-3d-demo` package exercises the complete initial
userspace path. With `INTEL_UHD.RCS_PROBE` enabled, its binary opens a render
BO through libdrm, submits the bounded RCS draw, waits for its fence, verifies
the orange pixels, activates its own DRM/KMS lease, presents through a dumb
framebuffer for five seconds, and returns to the console. It is intentionally
console-only and does not displace the desktop's graphical-session lease. RCS references only
permanent kernel-owned pages in this first ABI; after retirement the kernel
stages the 64x64 image into the program's BO. Run it as
`intel-uhd-3d-demo` from the framebuffer console with no desktop active.

## Desktop

The `desktop` program is a small compositor and window manager. It owns the
graphical session, composites application-owned ARGB surfaces through the
desktop pipe protocol, and presents only the framebuffer rectangle damaged by
an application, cursor movement, or window movement.

Appearance is configured through the signed-in user's `desktop.*` settings.
Start-menu launchers are individual files in `/config/desktop/launchers/`;
each has a `label`, `path`, and optional `args`, making the menu customizable
without changing the desktop binary. Click a window to focus it, drag its title
bar to move it, drag its bottom-right grip to resize it, and use its red
title-bar button to request that it close. F12 exits the desktop. The
`doomgeneric` package supports the protocol and retains fullscreen fallback
when no desktop is running.

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
