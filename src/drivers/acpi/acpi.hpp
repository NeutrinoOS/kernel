#pragma once

namespace acpi {

// Makes firmware tables available before the kernel heap exists.
bool initialize_tables();
// Starts the AML namespace and runtime services after core kernel init.
// Diagnostic command-line flags may stop initialization at individual stages.
bool initialize();
// True when initialize() successfully asked the firmware to leave legacy mode.
// Drivers backed by firmware-emulated legacy devices can use this to reprobe.
bool entered_acpi_mode();

}  // namespace acpi
