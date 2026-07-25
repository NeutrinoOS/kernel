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
### Debugging
You can use `make debug` to run in EFI mode with QEMU's debug mode enabled, allowing you to attach a debugger with e.g. `gdb -ex "target remote localhost:1234" -ex "symbol-file out/kernel.elf"`. The same considerations apply as with normal `make run.`
### Optimized builds
There's no support right now for optimized builds out of the box, but you can run something like `make clean all EXTRA_CFLAGS="-O3 -DNDEBUG=1"` to pass -O3 -DNDEBUG=1 into CFLAGs.

## Optional userspace dependencies
Some userspace features may optionally depend on third-party libraries that are not stored in the repository.

### BearSSL
If you want BearSSL available to userspace programs, provide your own BearSSL checkout at `userspace/deps/BearSSL`.

Once present, you can build the archive with:

```sh
make -C userspace bearssl
```

You can also build a packageable shared object with:

```sh
make -C userspace bearssl-shared
```

That produces `userspace/out/libbearssl.so.0` and stages `libbearssl.so.0`
plus the linker-name copy `libbearssl.so` under `userspace/library`. The
userspace install target copies staged shared libraries into `/library` on
the target filesystem, adjacent to `/binary` for package payloads and runtime
lookup. Current programs still link BearSSL statically until Neutrino grows
the runtime loader path for `DT_NEEDED` dependencies.

The userspace build is configured such that individual programs can optionally link against that .a, but BearSSL is probably not required for normal kernel or userspace builds unless a program explicitly depends on it.

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
