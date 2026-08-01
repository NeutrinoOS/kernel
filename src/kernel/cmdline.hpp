#pragma once

#include <stddef.h>

namespace kernel_cmdline {

struct Token {
    const char* name;
    const char* value;
};

// Copies and parses the bootloader-owned command line into kernel-owned
// storage. This must run before bootloader-reclaimable memory can be reused.
void initialize(const char* cmdline);

const char* raw();
bool truncated();

size_t token_count();
const Token* token_at(size_t index);

// Flags are bare tokens. Assignments are NAME=VALUE tokens, including empty
// values. value() returns the last assignment and value_at() preserves order.
bool has_flag(const char* name);
bool has_value(const char* name, const char* value);
size_t value_count(const char* name);
const char* value(const char* name);
const char* value_at(const char* name, size_t index);

}  // namespace kernel_cmdline
