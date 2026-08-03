#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audio_output {

struct Ops {
    bool (*available)();
    size_t (*write_pcm)(const void* data, size_t bytes);
    void (*drain)();
    void (*flush)();
    void (*set_paused)(bool paused);
    bool (*set_volume)(uint8_t percent);
    void (*get_status)(size_t& queued_bytes,
                       bool& running,
                       bool& paused,
                       uint8_t& volume);
};

bool register_provider(const char* name, const Ops* ops);
bool available();
size_t write_pcm(const void* data, size_t bytes);
void drain();
void flush();
void set_paused(bool paused);
bool set_volume(uint8_t percent);
void get_status(size_t& queued_bytes,
                bool& running,
                bool& paused,
                uint8_t& volume);

}  // namespace audio_output
