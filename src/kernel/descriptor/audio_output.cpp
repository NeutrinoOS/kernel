#include "kernel/descriptor.hpp"

#include <stdint.h>

#include "drivers/audio/audio_output.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/sync.hpp"
#include "lib/mem.hpp"

namespace descriptor {
namespace audio_output_descriptor {
namespace {

constexpr size_t kMaxStreams = 16;
constexpr size_t kStreamBufferBytes = 64 * 1024;
// The poll worker runs at 100 Hz and can be delayed by CPU-heavy clients such
// as video playback.  Keep substantially more than one scheduling slice in
// HDA so a delayed mixer pass produces a transient backlog, not a device-wide
// underrun, without pushing enough hidden latency to desynchronize clients'
// audio clocks.  At 48 kHz stereo PCM this is roughly 171 ms of lead time.
constexpr size_t kMixChunkBytes = 8 * 1024;
constexpr size_t kHardwareTargetBytes = 32 * 1024;

struct Stream {
    alignas(4) uint8_t buffer[kStreamBufferBytes];
    size_t head;
    size_t count;
    bool paused;
    bool in_use;
};

Stream g_streams[kMaxStreams]{};
alignas(4) int16_t g_mix_buffer[kMixChunkBytes / sizeof(int16_t)]{};
sync::SpinLock g_stream_lock;
bool g_poll_registered = false;

int16_t clamp_sample(int32_t sample) {
    if (sample > 32767) return 32767;
    if (sample < -32768) return -32768;
    return static_cast<int16_t>(sample);
}

void mix_service() {
    if (!audio_output::available()) return;

    // A lone stream with no software backlog is using the synchronous direct
    // path.  Leave HDA entirely alone in that case so the mixer poll cannot
    // contend with its writes or perturb its playback-position accounting.
    {
        sync::IrqLockGuard guard(g_stream_lock);
        size_t active_streams = 0;
        bool queued_samples = false;
        for (const auto& stream : g_streams) {
            if (!stream.in_use) continue;
            ++active_streams;
            queued_samples = queued_samples || stream.count != 0;
        }
        if (!queued_samples && active_streams <= 1) return;
    }

    size_t hardware_queued = 0;
    bool hardware_running = false;
    bool hardware_paused = false;
    uint8_t volume = 0;
    audio_output::get_status(hardware_queued,
                             hardware_running,
                             hardware_paused,
                             volume);
    (void)hardware_running;
    (void)volume;
    if (hardware_paused || hardware_queued >= kHardwareTargetBytes) return;

    size_t byte_budget = kHardwareTargetBytes - hardware_queued;
    if (byte_budget > kMixChunkBytes) byte_budget = kMixChunkBytes;
    byte_budget &= ~size_t{3};
    if (byte_budget == 0) return;

    size_t frames = 0;
    {
        sync::IrqLockGuard guard(g_stream_lock);
        for (const auto& stream : g_streams) {
            if (!stream.in_use || stream.paused) continue;
            size_t stream_frames = stream.count / 4;
            if (stream_frames > frames) frames = stream_frames;
        }
        const size_t frame_budget = byte_budget / 4;
        if (frames > frame_budget) frames = frame_budget;
        if (frames == 0) return;

        for (size_t frame = 0; frame < frames; ++frame) {
            int32_t left = 0;
            int32_t right = 0;
            for (auto& stream : g_streams) {
                if (!stream.in_use || stream.paused || stream.count < 4) {
                    continue;
                }
                int16_t samples[2];
                const size_t first = stream.head;
                if (first + 4 <= kStreamBufferBytes) {
                    memcpy(samples, stream.buffer + first, 4);
                } else {
                    uint8_t bytes[4];
                    for (size_t i = 0; i < 4; ++i) {
                        bytes[i] = stream.buffer[(first + i) %
                                                 kStreamBufferBytes];
                    }
                    memcpy(samples, bytes, 4);
                }
                stream.head = (stream.head + 4) % kStreamBufferBytes;
                stream.count -= 4;
                left += samples[0];
                right += samples[1];
            }
            g_mix_buffer[frame * 2] = clamp_sample(left);
            g_mix_buffer[frame * 2 + 1] = clamp_sample(right);
        }

        // This is the sole writer to the hardware provider.  The status check
        // above keeps the write below available ring capacity, so the provider
        // does not wait while holding its device lock.  Keep the stream lock
        // through submission so a concurrent final close cannot flush the
        // device between consuming these samples and writing them.
        (void)audio_output::write_pcm(g_mix_buffer, frames * 4);
    }
}

Stream* stream_for(DescriptorEntry& entry) {
    return static_cast<Stream*>(entry.subsystem_data);
}

}  // namespace

void close(DescriptorEntry& entry) {
    Stream* stream = stream_for(entry);
    bool any_streams = false;
    sync::IrqLockGuard guard(g_stream_lock);
    if (stream != nullptr) {
        stream->head = 0;
        stream->count = 0;
        stream->paused = false;
        stream->in_use = false;
    }
    for (const auto& candidate : g_streams) {
        if (candidate.in_use) {
            any_streams = true;
            break;
        }
    }
    // Do not let one client closing flush every other application's audio.
    // Once the final client closes, however, stop stale DMA playback.
    if (!any_streams) audio_output::flush();
}

int64_t read(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int64_t write(process::Task&, DescriptorEntry& entry, uint64_t address,
              uint64_t length, uint64_t offset) {
    if (offset != 0 || (length & 3u) != 0) return -1;
    if (length == 0) return 0;
    const auto* samples = reinterpret_cast<const uint8_t*>(address);
    Stream* stream = stream_for(entry);
    if (samples == nullptr || stream == nullptr) return -1;

    size_t bytes = static_cast<size_t>(length);
    {
        sync::IrqLockGuard guard(g_stream_lock);
        if (!stream->in_use) return -1;

        size_t active_streams = 0;
        for (const auto& candidate : g_streams) {
            if (candidate.in_use) ++active_streams;
        }
        // Preserve the original, proven path for the overwhelmingly common
        // single-client case.  In particular, SDL sees the real HDA queue and
        // the producer itself keeps that queue full instead of depending on
        // the coarse general-purpose poll worker.
        if (active_streams == 1 && stream->count == 0) {
            return static_cast<int64_t>(audio_output::write_pcm(samples, bytes));
        }

        size_t available = kStreamBufferBytes - stream->count;
        if (bytes > available) bytes = available;
        bytes &= ~size_t{3};
        size_t tail = (stream->head + stream->count) % kStreamBufferBytes;
        size_t first = kStreamBufferBytes - tail;
        if (first > bytes) first = bytes;
        memcpy(stream->buffer + tail, samples, first);
        if (bytes > first) {
            memcpy(stream->buffer, samples + first, bytes - first);
        }
        stream->count += bytes;
    }

    // Producer activity is a lower-jitter mixer wakeup than the 100 Hz
    // fallback poll.  The poller remains necessary to refill after hardware
    // consumption when all producers are temporarily asleep.
    mix_service();
    return static_cast<int64_t>(bytes);
}

int get_property(DescriptorEntry& entry, uint32_t property, void* out,
                 size_t size) {
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
        Stream* stream = stream_for(entry);
        if (stream == nullptr) return -1;
        size_t queued = 0;
        bool paused = false;
        bool direct = false;
        {
            sync::IrqLockGuard guard(g_stream_lock);
            if (!stream->in_use) return -1;
            queued = stream->count;
            paused = stream->paused;
            size_t active_streams = 0;
            for (const auto& candidate : g_streams) {
                if (candidate.in_use) ++active_streams;
            }
            direct = active_streams == 1 && queued == 0;
        }
        size_t hardware_queued = 0;
        bool hardware_running = false;
        bool hardware_paused = false;
        uint8_t volume = 0;
        audio_output::get_status(hardware_queued,
                                 hardware_running,
                                 hardware_paused,
                                 volume);
        (void)hardware_paused;
        auto* status = static_cast<descriptor_defs::AudioStatusInfo*>(out);
        status->queued_bytes = direct ? hardware_queued : queued;
        status->flags =
            (paused ? static_cast<uint32_t>(descriptor_defs::kAudioStatusPaused)
                    : 0u) |
            ((queued != 0 || hardware_running)
                 ? static_cast<uint32_t>(descriptor_defs::kAudioStatusRunning)
                 : 0u);
        status->volume = volume;
        return 0;
    }
    return -1;
}

int set_property(DescriptorEntry& entry, uint32_t property, const void* in,
                 size_t size) {
    if (property !=
            static_cast<uint32_t>(descriptor_defs::Property::AudioControl) ||
        in == nullptr || size < sizeof(descriptor_defs::AudioControlInfo))
        return -1;
    Stream* stream = stream_for(entry);
    if (stream == nullptr) return -1;
    const auto* control =
        static_cast<const descriptor_defs::AudioControlInfo*>(in);
    switch (control->command) {
        case descriptor_defs::kAudioCommandPause: {
            sync::IrqLockGuard guard(g_stream_lock);
            if (!stream->in_use) return -1;
            stream->paused = true;
            return 0;
        }
        case descriptor_defs::kAudioCommandResume: {
            sync::IrqLockGuard guard(g_stream_lock);
            if (!stream->in_use) return -1;
            stream->paused = false;
            return 0;
        }
        case descriptor_defs::kAudioCommandFlush: {
            sync::IrqLockGuard guard(g_stream_lock);
            if (!stream->in_use) return -1;
            stream->head = 0;
            stream->count = 0;
            return 0;
        }
        case descriptor_defs::kAudioCommandSetVolume:
            if (control->value < 0 || control->value > 100) return -1;
            return audio_output::set_volume(
                       static_cast<uint8_t>(control->value))
                       ? 0
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
    if (selector != 0 || !audio_output::available()) return false;

    Stream* stream = nullptr;
    {
        sync::IrqLockGuard guard(g_stream_lock);
        if (!g_poll_registered) {
            g_poll_registered = scheduler::register_realtime_poll(mix_service);
        }
        if (!g_poll_registered) return false;
        for (auto& candidate : g_streams) {
            if (candidate.in_use) continue;
            candidate.head = 0;
            candidate.count = 0;
            candidate.paused = false;
            candidate.in_use = true;
            stream = &candidate;
            break;
        }
    }
    if (stream == nullptr) return false;

    allocation.type = kTypeAudioOutput;
    allocation.flags = static_cast<uint64_t>(Flag::Writable) |
                       static_cast<uint64_t>(Flag::Device) |
                       static_cast<uint64_t>(Flag::CapStream);
    allocation.extended_flags = 0;
    allocation.has_extended_flags = false;
    allocation.object = nullptr;
    allocation.subsystem_data = stream;
    allocation.name = "mixed-pcm-out";
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
