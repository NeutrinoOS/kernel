#include "drivers/audio/audio_output.hpp"

#include "drivers/log/logging.hpp"

namespace audio_output {
namespace {

const Ops* g_ops = nullptr;

}  // namespace

bool register_provider(const char* name, const Ops* ops) {
    if (ops == nullptr || ops->available == nullptr ||
        ops->write_pcm == nullptr || ops->drain == nullptr ||
        ops->flush == nullptr || ops->set_paused == nullptr ||
        ops->set_volume == nullptr || ops->get_status == nullptr ||
        g_ops != nullptr) {
        return false;
    }
    g_ops = ops;
    log_message(LogLevel::Info,
                "Audio: registered output provider %s",
                name != nullptr ? name : "unnamed");
    return true;
}

bool available() { return g_ops != nullptr && g_ops->available(); }

size_t write_pcm(const void* data, size_t bytes) {
    return g_ops != nullptr ? g_ops->write_pcm(data, bytes) : 0;
}

void drain() {
    if (g_ops != nullptr) g_ops->drain();
}

void flush() {
    if (g_ops != nullptr) g_ops->flush();
}

void set_paused(bool paused) {
    if (g_ops != nullptr) g_ops->set_paused(paused);
}

bool set_volume(uint8_t percent) {
    return g_ops != nullptr && g_ops->set_volume(percent);
}

void get_status(size_t& queued_bytes,
                bool& running,
                bool& paused,
                uint8_t& volume) {
    if (g_ops != nullptr) {
        g_ops->get_status(queued_bytes, running, paused, volume);
        return;
    }
    queued_bytes = 0;
    running = false;
    paused = false;
    volume = 0;
}

}  // namespace audio_output
