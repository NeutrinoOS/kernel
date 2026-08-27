# === Build configuration ===
ARCH       := x86_64
CROSS      := x86_64-elf-
CC         := $(CROSS)g++
C_CC       := $(CROSS)gcc
LD         := $(CROSS)ld
AS         := nasm -f elf64
OBJCOPY    := $(CROSS)objcopy
KERNEL_VERSION ?= 0.0.0-dev
KERNEL_CMDLINE ?=

OUT_DIR    := out
BUILD_DIR  := build
SRC_DIR    := src

TARGET_ELF := $(OUT_DIR)/kernel.elf
TARGET_ISO := $(OUT_DIR)/neutrino.iso
TARGET_DISK_IMG ?= hdd.img
TARGET_DISK_SIZE ?= 4G

CFLAGS     := -std=c++20 -g -ffreestanding -O2 -Wall -Wextra -m64 -mno-red-zone -mno-sse -mno-mmx -mno-avx -mno-avx512f -mno-sse2 -fno-exceptions -fno-rtti -mcmodel=kernel $(EXTRA_CFLAGS) -Ishared/include -I$(SRC_DIR) -I$(SRC_DIR)/arch/$(ARCH) -I$(SRC_DIR)/third_party/uacpi/include
UACPI_CFLAGS := -std=c11 -g -ffreestanding -O2 -Wall -Wextra -m64 -mno-red-zone -mno-sse -mno-mmx -mno-avx -mno-avx512f -mno-sse2 -mcmodel=kernel $(EXTRA_CFLAGS) -Ishared/include -I$(SRC_DIR) -I$(SRC_DIR)/arch/$(ARCH) -I$(SRC_DIR)/third_party/uacpi/include
LDFLAGS    := -T $(SRC_DIR)/linker.ld -nostdlib

# === QEMU configuration ===
QEMU ?= qemu-system-x86_64
QEMU_BIOS ?= /usr/share/edk2/x64/OVMF.4m.fd
QEMU_NET_MAC ?= 52:54:00:12:34:56
QEMU_NET_BACKEND ?= user
QEMU_NET_DEVICE ?= e1000e
QEMU_STORAGE ?= ahci
QEMU_PRIMARY_IMG ?= $(TARGET_DISK_IMG)
QEMU_PRIMARY_IMG := $(if $(strip $(QEMU_PRIMARY_IMG)),$(QEMU_PRIMARY_IMG),$(TARGET_DISK_IMG))
QEMU_EXTRA_IDE_IMG ?=
QEMU_EXTRA_AHCI_IMG ?=
QEMU_XHCI ?= 0
QEMU_USB_STORAGE_IMG ?=
QEMU_HDA ?= 1
QEMU_AUDIO_DRIVER ?= pipewire
QEMU_AUDIO_OPTIONS ?= timer-period=5000,out.buffer-length=100000,out.latency=100000
QEMU_HOSTFWD ?= tcp::2222-:2222
QEMU_LIVE_BOOT_ARGS ?= -boot order=d
QEMU_INSTALLED_BOOT_ARGS ?= -boot order=c
QEMU_TAP_IFACE ?= tap0
QEMU_TAP_SCRIPT ?= no
QEMU_TAP_DOWN_SCRIPT ?= no
ifeq ($(QEMU_NET_BACKEND),tap)
QEMU_NET_ARGS := -netdev tap,id=net0,ifname=$(QEMU_TAP_IFACE),script=$(QEMU_TAP_SCRIPT),downscript=$(QEMU_TAP_DOWN_SCRIPT) \
		-device $(QEMU_NET_DEVICE),netdev=net0,mac=$(QEMU_NET_MAC)
else
QEMU_NETDEV_USER := -netdev user,id=net0
ifneq ($(strip $(QEMU_HOSTFWD)),)
QEMU_NETDEV_USER := $(QEMU_NETDEV_USER),hostfwd=$(QEMU_HOSTFWD)
endif
QEMU_NET_ARGS := $(QEMU_NETDEV_USER) \
		-device $(QEMU_NET_DEVICE),netdev=net0,mac=$(QEMU_NET_MAC)
endif
ifeq ($(QEMU_STORAGE),ahci)
QEMU_STORAGE_ARGS := -device ahci,id=ahci0 \
		-drive file=$(QEMU_PRIMARY_IMG),format=raw,if=none,id=disk0 \
		-device ide-hd,drive=disk0,bus=ahci0.0
QEMU_ROOT_DEVICE := AHCI_0_0
ifneq ($(strip $(QEMU_EXTRA_AHCI_IMG)),)
QEMU_AHCI_EXTRA_ARGS := -drive file=$(QEMU_EXTRA_AHCI_IMG),format=raw,if=none,id=ahci_extra_disk0 \
		-device ide-hd,drive=ahci_extra_disk0,bus=ahci0.1
endif
else
QEMU_STORAGE_ARGS := -drive file=$(QEMU_PRIMARY_IMG),format=raw,if=ide,index=0
QEMU_ROOT_DEVICE := IDE_PM_0
ifneq ($(strip $(QEMU_EXTRA_AHCI_IMG)),)
QEMU_AHCI_EXTRA_ARGS := -device ahci,id=ahci_extra0 \
		-drive file=$(QEMU_EXTRA_AHCI_IMG),format=raw,if=none,id=ahci_extra_disk0 \
		-device ide-hd,drive=ahci_extra_disk0,bus=ahci_extra0.0
endif
endif
ifneq ($(strip $(QEMU_EXTRA_IDE_IMG)),)
QEMU_IDE_EXTRA_ARGS := -drive file=$(QEMU_EXTRA_IDE_IMG),format=raw,if=ide,index=2
endif
QEMU_STORAGE_ARGS += $(QEMU_IDE_EXTRA_ARGS) $(QEMU_AHCI_EXTRA_ARGS)
ifeq ($(QEMU_XHCI),1)
QEMU_USB_ARGS := -device qemu-xhci,id=xhci0
ifneq ($(strip $(QEMU_USB_STORAGE_IMG)),)
QEMU_USB_ARGS += -drive file=$(QEMU_USB_STORAGE_IMG),format=raw,if=none,id=usb_disk0 \
		-device usb-storage,drive=usb_disk0,bus=xhci0.0
endif
endif
ifeq ($(QEMU_HDA),1)
QEMU_AUDIO_ARGS := -audiodev $(QEMU_AUDIO_DRIVER),id=audio0,$(QEMU_AUDIO_OPTIONS) \
		-device ich9-intel-hda -device hda-output,audiodev=audio0
endif
QEMU_BASE_ARGS := -m 4G -serial stdio \
		-smp 2 -bios $(QEMU_BIOS) \
		$(QEMU_STORAGE_ARGS) \
		-enable-kvm -display sdl \
		-machine pc -cpu max \
		$(QEMU_USB_ARGS) \
		$(QEMU_AUDIO_ARGS) \
		$(QEMU_NET_ARGS)
QEMU_COMMON_ARGS := $(QEMU_BASE_ARGS) -cdrom $(TARGET_ISO)
QEMU_INSTALLED_ARGS := $(QEMU_BASE_ARGS)
QEMU_DEBUG_ARGS := -d int \
		-monitor unix:./qemu-monitor-socket,server,nowait -no-shutdown -no-reboot
QEMU_DEBUG_WAIT_ARGS := -s -S

TARGET_ISO_RAMFS := $(OUT_DIR)/neutrino_ramfs.iso
ISO_ROOT_RAMFS := $(OUT_DIR)/iso_root_ramfs
LIVE_ROOTFS_IMG ?= $(OUT_DIR)/live_rootfs.img
# Minimum image size. The rootfs recipe grows beyond this automatically when
# staged packages and their on-image repository need more room.
LIVE_ROOTFS_SIZE ?= 128M
LIVE_ROOTFS_HEADROOM ?= 32M
NEUTRINO_PACKAGES_ROOT ?= ../neutrino-packages
LOCAL_LIVE_PACKAGES := $(if $(wildcard $(NEUTRINO_PACKAGES_ROOT)/local-files/package/manifest.toml),local-files,)
DEFAULT_LIVE_PACKAGES := neutrino-installer neutrino-live neutrino-drivers $(LOCAL_LIVE_PACKAGES)
LIVE_PACKAGES ?= $(DEFAULT_LIVE_PACKAGES)
LIVE_PACKAGE_NAMES = $(shell $(NEUTRINO_PACKAGES_ROOT)/resolve-package-deps.sh $(LIVE_PACKAGES))
LIVE_PACKAGE_ZIPS = $(foreach package,$(LIVE_PACKAGE_NAMES),$(NEUTRINO_PACKAGES_ROOT)/$(package)/out/$(package).zip)
LIVE_REPO_DIR := $(OUT_DIR)/live-repo
LIVE_ROOTFS_STAGE := $(BUILD_DIR)/live-rootfs
LIVE_ESP_IMG ?= $(OUT_DIR)/esp.img
LIVE_ESP_SIZE ?= 64M

# === Source discovery ===
KERNEL_MODULE_NAMES := e1000e.ko virtio-net.ko intel-hda.ko intel-uhd-gemini-lake.ko
KERNEL_MODULE_LOADS := $(OUT_DIR)/modules/loads.txt
SRC_CPP_ALL := $(shell find $(SRC_DIR) -type f -name '*.cpp')
KERNEL_VERSION_CPP := $(SRC_DIR)/kernel/version.cpp
KERNEL_VERSION_OBJ := $(BUILD_DIR)/kernel/version-$(KERNEL_VERSION).o
KERNEL_VERSION_MARKER := $(BUILD_DIR)/kernel/.version-$(KERNEL_VERSION)
OTHER_KERNEL_VERSION_MARKERS := $(filter-out $(KERNEL_VERSION_MARKER),$(wildcard $(BUILD_DIR)/kernel/.version-*))
SRC_CPP := $(filter-out $(KERNEL_VERSION_CPP),$(SRC_CPP_ALL))
UACPI_C := $(shell find $(SRC_DIR)/third_party/uacpi/source -maxdepth 1 -type f -name '*.c')
SRC_ASM := $(shell find $(SRC_DIR) -type f -name '*.S')
OBJ     := $(SRC_CPP:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o) \
           $(UACPI_C:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o) \
           $(SRC_ASM:$(SRC_DIR)/%.S=$(BUILD_DIR)/%.o) \
           $(KERNEL_VERSION_OBJ)
KERNEL_SIMD_CPP ?=
KERNEL_SIMD_OBJ := $(KERNEL_SIMD_CPP:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
KERNEL_SIMD_CFLAGS := $(filter-out -mno-mmx -mno-sse -mno-sse2,$(CFLAGS)) -mmmx -msse -msse2

# === Default target ===
all: $(TARGET_ELF)

# === Object build rules ===
$(KERNEL_SIMD_OBJ): CFLAGS := $(KERNEL_SIMD_CFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "[C++] $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_VERSION_OBJ): $(KERNEL_VERSION_CPP) $(SRC_DIR)/kernel/version.hpp
	@mkdir -p $(dir $@)
	@echo "[C++] $< ($(KERNEL_VERSION))"
	$(CC) $(CFLAGS) -DNEUTRINO_KERNEL_VERSION='"$(KERNEL_VERSION)"' -c $< -o $@

$(KERNEL_VERSION_MARKER):
	@mkdir -p $(dir $@)
	@rm -f $(OTHER_KERNEL_VERSION_MARKERS)
	@touch $@

$(BUILD_DIR)/third_party/uacpi/source/%.o: $(SRC_DIR)/third_party/uacpi/source/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]   $<"
	$(C_CC) $(UACPI_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	@echo "[ASM]  $<"
	$(AS) $< -o $@

$(KERNEL_MODULE_LOADS): Makefile
	@mkdir -p $(dir $@)
	@echo "[MODS] $@"
	@rm -f $@
	@for module in $(KERNEL_MODULE_NAMES); do \
		printf '%s\n' "$$module" >> $@; \
	done

# === Link kernel ELF ===
$(TARGET_ELF): $(KERNEL_VERSION_MARKER) $(OBJ)
	@mkdir -p $(OUT_DIR)
	@echo "[LD]   $@"
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

# === Grab Limine if it's missing ===
LIMINE_DIR := limine

$(LIMINE_DIR):
	@echo "[Limine] Cloning Limine..."
	git clone https://codeberg.org/Limine/Limine.git $(LIMINE_DIR) --branch=v10.x-binary --depth=1
	cd limine
	make -C limine

# === Build bootable UEFI ISO ===
ISO_ROOT := $(OUT_DIR)/iso_root

$(TARGET_ISO): $(TARGET_ELF) $(LIMINE_DIR) $(LIVE_ROOTFS_IMG) force-live-esp
	@echo "[ISO] Building $@"

	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot
	cp -v out/kernel.elf $(ISO_ROOT)/boot/kernel.elf
	cp -v $(LIVE_ROOTFS_IMG) $(ISO_ROOT)/boot/rootfs.img
	cp -v $(LIVE_ESP_IMG) $(ISO_ROOT)/boot/esp.img
	mkdir -p $(ISO_ROOT)/boot/limine
	cp -v $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	
	printf 'timeout: 1\n\n' > $(ISO_ROOT)/limine.conf
	printf '/Neutrino Live\n' >> $(ISO_ROOT)/limine.conf
	printf '    protocol: limine\n' >> $(ISO_ROOT)/limine.conf
	printf '    path: boot():/boot/kernel.elf\n' >> $(ISO_ROOT)/limine.conf
	@if [[ -n "$(strip $(KERNEL_CMDLINE))" ]]; then \
		printf '    cmdline: %s\n' "$(KERNEL_CMDLINE)" >> $(ISO_ROOT)/limine.conf; \
	fi
	printf '    module_path: boot():/boot/rootfs.img\n' >> $(ISO_ROOT)/limine.conf
	printf '    module_cmdline: rootfs\n' >> $(ISO_ROOT)/limine.conf
	printf '    module_path: boot():/boot/esp.img\n' >> $(ISO_ROOT)/limine.conf
	printf '    module_cmdline: esp\n\n' >> $(ISO_ROOT)/limine.conf

	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp -v $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	cp -v $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/

	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        $(ISO_ROOT) -o $(TARGET_ISO)
	
	$(LIMINE_DIR)/limine bios-install $(TARGET_ISO)

$(TARGET_ISO_RAMFS): $(TARGET_ELF) $(LIMINE_DIR) $(LIVE_ROOTFS_IMG) force-live-esp
	@echo "[ISO] Building $@ (RAMFS)"

	rm -rf $(ISO_ROOT_RAMFS)
	mkdir -p $(ISO_ROOT_RAMFS)/boot
	cp -v out/kernel.elf $(ISO_ROOT_RAMFS)/boot/kernel.elf
	mkdir -p $(ISO_ROOT_RAMFS)/boot/limine
	cp -v $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT_RAMFS)/boot/limine/

	cp -v $(LIVE_ROOTFS_IMG) $(ISO_ROOT_RAMFS)/boot/rootfs.img
	cp -v $(LIVE_ESP_IMG) $(ISO_ROOT_RAMFS)/boot/esp.img

	printf 'timeout: 1\n\n' > $(ISO_ROOT_RAMFS)/limine.conf
	printf '/Neutrino Live\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    protocol: limine\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    path: boot():/boot/kernel.elf\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	@if [[ -n "$(strip $(KERNEL_CMDLINE))" ]]; then \
		printf '    cmdline: %s\n' "$(KERNEL_CMDLINE)" >> $(ISO_ROOT_RAMFS)/limine.conf; \
	fi
	printf '    module_path: boot():/boot/rootfs.img\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    module_cmdline: rootfs\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    module_path: boot():/boot/esp.img\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    module_cmdline: esp\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '\n' >> $(ISO_ROOT_RAMFS)/limine.conf

	mkdir -p $(ISO_ROOT_RAMFS)/EFI/BOOT
	cp -v $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT_RAMFS)/EFI/BOOT/
	cp -v $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT_RAMFS)/EFI/BOOT/

	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        $(ISO_ROOT_RAMFS) -o $(TARGET_ISO_RAMFS)
	
	$(LIMINE_DIR)/limine bios-install $(TARGET_ISO_RAMFS)

run: $(TARGET_ISO) target-disk
	$(QEMU) $(QEMU_COMMON_ARGS) $(QEMU_LIVE_BOOT_ARGS)

run-live: run

run-installed: target-disk
	$(QEMU) $(QEMU_INSTALLED_ARGS) $(QEMU_INSTALLED_BOOT_ARGS)

# === Run but wait for debugger to attach ===
debug: $(TARGET_ISO) target-disk
	$(QEMU) $(QEMU_COMMON_ARGS) $(QEMU_LIVE_BOOT_ARGS) $(QEMU_DEBUG_ARGS) $(QEMU_DEBUG_WAIT_ARGS)

debug-installed: target-disk
	$(QEMU) $(QEMU_INSTALLED_ARGS) $(QEMU_INSTALLED_BOOT_ARGS) $(QEMU_DEBUG_ARGS) $(QEMU_DEBUG_WAIT_ARGS)

# === Run with -d int, do not wait for debugger to attach ===
debug-nostop: $(TARGET_ISO) target-disk
	$(QEMU) $(QEMU_COMMON_ARGS) $(QEMU_LIVE_BOOT_ARGS) $(QEMU_DEBUG_ARGS)

debug-installed-nostop: target-disk
	$(QEMU) $(QEMU_INSTALLED_ARGS) $(QEMU_INSTALLED_BOOT_ARGS) $(QEMU_DEBUG_ARGS)

# === Utility targets ===
iso: $(TARGET_ISO)
	@echo ISO created at $(TARGET_ISO)

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)

iso-ramfs: $(TARGET_ISO_RAMFS)
	@echo ISO with RAMFS created at $(TARGET_ISO_RAMFS)

.PHONY: userspace-rootfs
userspace-rootfs: live-rootfs

.PHONY: live-rootfs
live-rootfs: $(LIVE_ROOTFS_IMG)
	@echo "Live rootfs ready at $(LIVE_ROOTFS_IMG)"

.PHONY: live-esp
live-esp: $(LIVE_ESP_IMG)
	@echo "Installer ESP image ready at $(LIVE_ESP_IMG)"

.PHONY: force-live-esp
force-live-esp: $(LIVE_ESP_IMG)

.PHONY: userspace-sdk check-live-packages live-package-archives print-live-packages
userspace-sdk:
	$(MAKE) -C userspace newlib-sdk

check-live-packages:
	@$(NEUTRINO_PACKAGES_ROOT)/resolve-package-deps.sh $(LIVE_PACKAGES) >/dev/null

print-live-packages: check-live-packages
	@echo "Requested live packages: $(LIVE_PACKAGES)"
	@echo "Resolved live packages:  $(LIVE_PACKAGE_NAMES)"

live-package-archives: check-live-packages userspace-sdk $(KERNEL_MODULE_LOADS)
	@set -euo pipefail; \
	for package in $(LIVE_PACKAGE_NAMES); do \
		$(MAKE) -C "$(NEUTRINO_PACKAGES_ROOT)/$$package" package; \
	done

$(LIVE_ROOTFS_IMG): live-package-archives $(NEUTRINO_PACKAGES_ROOT)/build-local-repo.sh $(NEUTRINO_PACKAGES_ROOT)/install-packages-root.sh
	@mkdir -p $(dir $@)
	rm -rf $(LIVE_REPO_DIR) $(LIVE_ROOTFS_STAGE)
	mkdir -p $(LIVE_REPO_DIR) $(LIVE_ROOTFS_STAGE)/packages $(LIVE_ROOTFS_STAGE)/system
	$(NEUTRINO_PACKAGES_ROOT)/build-local-repo.sh $(LIVE_REPO_DIR) $(LIVE_PACKAGE_ZIPS)
	$(NEUTRINO_PACKAGES_ROOT)/install-packages-root.sh $(LIVE_ROOTFS_STAGE) $(LIVE_PACKAGE_ZIPS)
	mkdir -p $(LIVE_ROOTFS_STAGE)/modules
	cp $(KERNEL_MODULE_LOADS) $(LIVE_ROOTFS_STAGE)/modules/loads.txt
	cp -a $(LIVE_REPO_DIR)/. $(LIVE_ROOTFS_STAGE)/packages/
	# The live medium uses an ephemeral RAM write overlay at runtime. Seed it
	# with an explicit valid, empty v4 credential store so init can distinguish
	# intentional bootstrap mode from a missing, truncated, or corrupt database.
	printf '\125\104\124\116\004\000\200\000\000\000\000\000\001\000\000\000\000\000\000\000\001\000\000\000\000\000\000\000\000\000\000\000' > $(LIVE_ROOTFS_STAGE)/system/users.ntd
	rm -f $@
	@set -euo pipefail; \
		staged_bytes=$$(du -sb $(LIVE_ROOTFS_STAGE) | cut -f1); \
		minimum_bytes=$$(numfmt --from=iec "$(LIVE_ROOTFS_SIZE)"); \
		headroom_bytes=$$(numfmt --from=iec "$(LIVE_ROOTFS_HEADROOM)"); \
		required_bytes=$$((staged_bytes + staged_bytes / 4 + headroom_bytes)); \
		quantum=$$((16 * 1024 * 1024)); \
		required_bytes=$$(((required_bytes + quantum - 1) / quantum * quantum)); \
		image_bytes=$$minimum_bytes; \
		if ((required_bytes > image_bytes)); then image_bytes=$$required_bytes; fi; \
		echo "Live rootfs: staged $$staged_bytes bytes, allocating $$image_bytes bytes"; \
		truncate -s $$image_bytes $@
	mkfs.fat -F 32 --mbr=y $@
	mcopy -s -i $@ $(LIVE_ROOTFS_STAGE)/* ::/

$(LIVE_ESP_IMG): $(TARGET_ELF) $(LIMINE_DIR)
	@mkdir -p $(dir $@)
	rm -f $@ $(OUT_DIR)/esp-limine.conf
	truncate -s $(LIVE_ESP_SIZE) $@
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot
	mcopy -i $@ $(LIMINE_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(TARGET_ELF) ::/boot/kernel.elf
	printf 'timeout: 1\n\n' > $(OUT_DIR)/esp-limine.conf
	printf '/Neutrino\n' >> $(OUT_DIR)/esp-limine.conf
	printf '    protocol: limine\n' >> $(OUT_DIR)/esp-limine.conf
	printf '    path: boot():/boot/kernel.elf\n' >> $(OUT_DIR)/esp-limine.conf
	printf '    cmdline: ROOT=EMMC_0_1\n' >> $(OUT_DIR)/esp-limine.conf
	mcopy -i $@ $(OUT_DIR)/esp-limine.conf ::/limine.conf

.PHONY: target-disk
target-disk: $(TARGET_DISK_IMG)
	@echo "Target disk ready at $(TARGET_DISK_IMG)"

$(TARGET_DISK_IMG):
	truncate -s $(TARGET_DISK_SIZE) $(TARGET_DISK_IMG)

.PHONY: all clean iso iso-ramfs run run-live run-installed debug debug-installed debug-nostop debug-installed-nostop userspace-rootfs live-rootfs live-esp
