#pragma once

#include <stddef.h>
#include <stdint.h>

#include "process.hpp"

namespace loader {

struct ProgramImage {
    const uint8_t* data;
    size_t size;
    uint64_t entry_offset;
};

bool load_into_process(const ProgramImage& image, process::Task& proc);
bool load_file_into_process(const char* path, process::Task& proc);

uint64_t dynamic_load(process::Task& proc, const char* path);
uint64_t dynamic_symbol(process::Task& proc,
                        uint64_t handle,
                        const char* symbol);
bool dynamic_close(process::Task& proc, uint64_t handle);
void release_dynamic_objects(process::Task& proc);

}  // namespace loader
