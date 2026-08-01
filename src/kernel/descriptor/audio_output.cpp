#include "kernel/descriptor.hpp"

#include "drivers/audio/hda.hpp"

namespace descriptor {
namespace audio_output_descriptor {

void close(DescriptorEntry&) { hda::drain(); }

int64_t read(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int64_t write(process::Task&, DescriptorEntry&, uint64_t address,
              uint64_t length, uint64_t offset) {
    if (offset != 0 || (length & 3u) != 0) return -1;
    if (length == 0) return 0;
    const void* samples = reinterpret_cast<const void*>(address);
    if (samples == nullptr) return -1;
    return static_cast<int64_t>(hda::write_pcm(samples, static_cast<size_t>(length)));
}

int get_property(DescriptorEntry&, uint32_t property, void* out, size_t size) {
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::AudioFormat)) {
        if (out == nullptr || size < sizeof(descriptor_defs::AudioFormatInfo))
            return -1;
        auto* format = static_cast<descriptor_defs::AudioFormatInfo*>(out);
        *format = descriptor_defs::AudioFormatInfo{48000, 2, 16, 4, 0};
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::AudioStatus)) {
        if (out == nullptr || size < sizeof(descriptor_defs::AudioStatusInfo))
            return -1;
        size_t queued = 0;
        bool running = false;
        bool paused = false;
        uint8_t volume = 0;
        hda::get_status(queued, running, paused, volume);
        auto* status = static_cast<descriptor_defs::AudioStatusInfo*>(out);
        status->queued_bytes = queued;
        status->flags =
            (paused ? static_cast<uint32_t>(descriptor_defs::kAudioStatusPaused)
                    : 0u) |
            (running
                 ? static_cast<uint32_t>(descriptor_defs::kAudioStatusRunning)
                 : 0u);
        status->volume = volume;
        return 0;
    }
    return -1;
}

int set_property(DescriptorEntry&, uint32_t property, const void* in,
                 size_t size) {
    if (property !=
            static_cast<uint32_t>(descriptor_defs::Property::AudioControl) ||
        in == nullptr || size < sizeof(descriptor_defs::AudioControlInfo))
        return -1;
    const auto* control =
        static_cast<const descriptor_defs::AudioControlInfo*>(in);
    switch (control->command) {
        case descriptor_defs::kAudioCommandPause:
            hda::set_paused(true);
            return 0;
        case descriptor_defs::kAudioCommandResume:
            hda::set_paused(false);
            return 0;
        case descriptor_defs::kAudioCommandFlush:
            hda::flush();
            return 0;
        case descriptor_defs::kAudioCommandSetVolume:
            if (control->value < 0 || control->value > 100) return -1;
            return hda::set_volume(static_cast<uint8_t>(control->value)) ? 0
                                                                          : -1;
        default:
            return -1;
    }
}

const Ops kOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = set_property,
};

bool open(process::Task&, uint64_t selector, uint64_t, uint64_t,
          Allocation& allocation) {
    if (selector != 0 || !hda::available()) return false;
    allocation.type = kTypeAudioOutput;
    allocation.flags = static_cast<uint64_t>(Flag::Writable) |
                       static_cast<uint64_t>(Flag::Device) |
                       static_cast<uint64_t>(Flag::CapStream);
    allocation.extended_flags = 0;
    allocation.has_extended_flags = false;
    allocation.object = nullptr;
    allocation.subsystem_data = nullptr;
    allocation.name = "hda-pcm-out";
    allocation.ops = &kOps;
    allocation.ext = nullptr;
    allocation.close = close;
    return true;
}

}  // namespace audio_output_descriptor

bool register_audio_output_descriptor() {
    return register_type(kTypeAudioOutput, audio_output_descriptor::open,
                         &audio_output_descriptor::kOps);
}

}  // namespace descriptor
