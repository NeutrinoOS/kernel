#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

class Console;

namespace settings {

void set_storage_root(const char* root_mount_name);
bool load_from_disk();
bool save_to_disk();

const char* get_string(const char* key);
// Copies the value while the registry is locked. out_length includes the
// terminating NUL on success.
bool copy_string(const char* key,
                 char* out,
                 size_t out_size,
                 size_t& out_length);
bool copy_key(size_t index, char* out, size_t out_size, size_t& out_length);
bool get_u32(const char* key, uint32_t& out);
bool set_string(const char* key, const char* value);
// Updates the in-memory registry only when the new value can be persisted.
bool set_string_and_save(const char* key, const char* value);
bool copy_user_string(uint64_t machine_id,
                      uint64_t local_id,
                      const char* key,
                      char* out,
                      size_t out_size,
                      size_t& out_length);
bool copy_user_key(uint64_t machine_id,
                   uint64_t local_id,
                   size_t index,
                   char* out,
                   size_t out_size,
                   size_t& out_length);
bool set_user_string_and_save(uint64_t machine_id,
                              uint64_t local_id,
                              const char* key,
                              const char* value);
bool set_u32(const char* key, uint32_t value);

bool apply_console_preferences(Console& console);
void persist_console_scale(uint32_t scale);
void persist_console_font(const descriptor_defs::ConsoleFont& font,
                          const uint8_t* data);

}  // namespace settings
