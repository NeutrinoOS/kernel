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

}  // namespace loader
