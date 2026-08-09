#include "time.hpp"

#include "arch/x86_64/io.hpp"
#include "../drivers/log/logging.hpp"

#include <uacpi/acpi.h>
#include <uacpi/tables.h>

namespace {

constexpr uint16_t kCmosAddressPort = 0x70;
constexpr uint16_t kCmosDataPort = 0x71;
constexpr uint8_t kRegisterSeconds = 0x00;
constexpr uint8_t kRegisterMinutes = 0x02;
constexpr uint8_t kRegisterHours = 0x04;
constexpr uint8_t kRegisterWeekday = 0x06;
constexpr uint8_t kRegisterDay = 0x07;
constexpr uint8_t kRegisterMonth = 0x08;
constexpr uint8_t kRegisterYear = 0x09;
constexpr uint8_t kRegisterStatusA = 0x0A;
constexpr uint8_t kRegisterStatusB = 0x0B;
constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
constexpr uint32_t kPitInputFrequency = 1193182u;
constexpr uint16_t kPitCalibrationCount = 0xFFFFu;
constexpr uint16_t kPitChannel2Port = 0x42;
constexpr uint16_t kPitCommandPort = 0x43;
constexpr uint16_t kPitSpeakerPort = 0x61;

struct RtcSample {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t weekday;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    uint8_t status_b;
};

uint32_t g_time_seq = 0;
uint64_t g_unix_seconds = 0;
uint32_t g_nanoseconds = 0;
uint32_t g_tick_hz = 100;
uint32_t g_tick_remainder = 0;
uint32_t g_tick_nanos_floor = 10000000;
uint32_t g_tick_nanos_remainder = 0;
uint64_t g_tick_count = 0;
uint64_t g_uptime_nanoseconds = 0;
bool g_initialized = false;
uint64_t g_tsc_base = 0;
uint64_t g_tsc_frequency_hz = 0;
uint64_t g_last_monotonic_nanoseconds = 0;

struct CpuidResult {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

CpuidResult cpuid(uint32_t leaf) {
    CpuidResult result{};
    asm volatile("cpuid"
                 : "=a"(result.eax), "=b"(result.ebx),
                   "=c"(result.ecx), "=d"(result.edx)
                 : "a"(leaf), "c"(0));
    return result;
}

uint64_t read_tsc() {
    uint32_t low = 0;
    uint32_t high = 0;
    // LFENCE orders the timestamp read after preceding loads on x86-64.
    asm volatile("lfence\n"
                 "rdtsc"
                 : "=a"(low), "=d"(high)
                 :
                 : "memory");
    return (static_cast<uint64_t>(high) << 32) | low;
}

uint64_t calibrate_tsc_with_pit() {
    const uint8_t original_speaker = inb(kPitSpeakerPort);

    // Keep the speaker silent and hold channel 2's gate low while loading a
    // one-shot count. Channel 0 remains untouched and continues driving IRQ0.
    outb(kPitSpeakerPort,
         static_cast<uint8_t>(original_speaker & ~0x03u));
    outb(kPitCommandPort, 0xB0u);
    outb(kPitChannel2Port,
         static_cast<uint8_t>(kPitCalibrationCount & 0xFFu));
    outb(kPitChannel2Port,
         static_cast<uint8_t>(kPitCalibrationCount >> 8));

    const uint64_t start = read_tsc();
    outb(kPitSpeakerPort,
         static_cast<uint8_t>((original_speaker & ~0x02u) | 0x01u));

    bool elapsed = false;
    for (uint32_t spin = 0; spin < 10000000u; ++spin) {
        if ((inb(kPitSpeakerPort) & 0x20u) != 0) {
            elapsed = true;
            break;
        }
        asm volatile("pause");
    }
    const uint64_t end = read_tsc();
    outb(kPitSpeakerPort, original_speaker);
    if (!elapsed || end <= start) return 0;

    return ((end - start) * kPitInputFrequency +
            kPitCalibrationCount / 2u) /
           kPitCalibrationCount;
}

uint64_t detect_tsc_frequency() {
    constexpr uint64_t kMaximumPlausibleTscHz = 10000000000ull;
    const CpuidResult max_basic = cpuid(0);
    if (max_basic.eax < 1u || (cpuid(1u).edx & (1u << 4)) == 0) {
        return 0;
    }

    if (max_basic.eax >= 0x15u) {
        const CpuidResult ratio = cpuid(0x15u);
        if (ratio.eax != 0 && ratio.ebx != 0 && ratio.ecx != 0) {
            const uint64_t frequency_hz =
                (static_cast<uint64_t>(ratio.ecx) * ratio.ebx) / ratio.eax;
            if (frequency_hz <= kMaximumPlausibleTscHz) {
                return frequency_hz;
            }
        }
    }

    // CPUID.16H reports the nominal core frequency in MHz. It is less exact
    // than the crystal-clock ratio, but avoids a calibration delay when leaf
    // 15H is incomplete.
    if (max_basic.eax >= 0x16u) {
        const uint32_t base_mhz = cpuid(0x16u).eax & 0xFFFFu;
        const uint64_t frequency_hz =
            static_cast<uint64_t>(base_mhz) * 1000000ull;
        if (frequency_hz != 0 && frequency_hz <= kMaximumPlausibleTscHz) {
            return frequency_hz;
        }
    }

    const uint64_t calibrated_hz = calibrate_tsc_with_pit();
    return calibrated_hz <= kMaximumPlausibleTscHz ? calibrated_hz : 0;
}

bool clock_available() {
    return __atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE);
}

uint8_t rtc_century_register() {
    acpi_fadt* fadt = nullptr;
    if (uacpi_table_fadt(&fadt) == UACPI_STATUS_OK && fadt != nullptr) {
        return fadt->century;
    }
    return 0;
}

uint8_t read_cmos(uint8_t reg) {
    outb(kCmosAddressPort, static_cast<uint8_t>(0x80u | reg));
    io_wait();
    return inb(kCmosDataPort);
}

bool update_in_progress() {
    return (read_cmos(kRegisterStatusA) & 0x80u) != 0;
}

void wait_for_rtc_update() {
    for (uint32_t spin = 0; spin < 100000u; ++spin) {
        if (!update_in_progress()) {
            return;
        }
    }
}

RtcSample read_rtc_sample(uint8_t century_register) {
    RtcSample sample{};
    sample.second = read_cmos(kRegisterSeconds);
    sample.minute = read_cmos(kRegisterMinutes);
    sample.hour = read_cmos(kRegisterHours);
    sample.weekday = read_cmos(kRegisterWeekday);
    sample.day = read_cmos(kRegisterDay);
    sample.month = read_cmos(kRegisterMonth);
    sample.year = read_cmos(kRegisterYear);
    sample.century = century_register != 0 ? read_cmos(century_register) : 0;
    sample.status_b = read_cmos(kRegisterStatusB);
    return sample;
}

bool samples_match(const RtcSample& lhs, const RtcSample& rhs) {
    return lhs.second == rhs.second &&
           lhs.minute == rhs.minute &&
           lhs.hour == rhs.hour &&
           lhs.weekday == rhs.weekday &&
           lhs.day == rhs.day &&
           lhs.month == rhs.month &&
           lhs.year == rhs.year &&
           lhs.century == rhs.century &&
           lhs.status_b == rhs.status_b;
}

uint8_t bcd_to_binary(uint8_t value) {
    return static_cast<uint8_t>(((value >> 4) & 0x0Fu) * 10u + (value & 0x0Fu));
}

bool normalize_sample(RtcSample& sample) {
    bool binary_mode = (sample.status_b & 0x04u) != 0;
    bool hour_24_mode = (sample.status_b & 0x02u) != 0;
    bool pm = (sample.hour & 0x80u) != 0;

    uint8_t hour = static_cast<uint8_t>(sample.hour & 0x7Fu);
    if (!binary_mode) {
        sample.second = bcd_to_binary(sample.second);
        sample.minute = bcd_to_binary(sample.minute);
        hour = bcd_to_binary(hour);
        sample.day = bcd_to_binary(sample.day);
        sample.month = bcd_to_binary(sample.month);
        sample.year = bcd_to_binary(sample.year);
        sample.century = sample.century != 0 ? bcd_to_binary(sample.century) : 0;
    }

    if (!hour_24_mode) {
        if (pm) {
            if (hour < 12) {
                hour = static_cast<uint8_t>(hour + 12);
            }
        } else if (hour == 12) {
            hour = 0;
        }
    }

    sample.hour = hour;

    if (sample.second > 59 || sample.minute > 59 || sample.hour > 23 ||
        sample.day == 0 || sample.day > 31 || sample.month == 0 ||
        sample.month > 12) {
        return false;
    }
    return true;
}

int64_t days_from_civil(int32_t year, uint32_t month, uint32_t day) {
    year -= month <= 2 ? 1 : 0;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(year - era * 400);
    const int32_t month_index =
        static_cast<int32_t>(month) + (month > 2 ? -3 : 9);
    const uint32_t doy =
        (153u * static_cast<uint32_t>(month_index) + 2u) / 5u +
        day - 1u;
    const uint32_t doe =
        yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<int64_t>(era) * 146097ll +
           static_cast<int64_t>(doe) - 719468ll;
}

void civil_from_days(int64_t days,
                     uint16_t& year,
                     uint8_t& month,
                     uint8_t& day) {
    days += 719468ll;
    const int64_t era = (days >= 0 ? days : days - 146096ll) / 146097ll;
    const uint32_t doe = static_cast<uint32_t>(days - era * 146097ll);
    const uint32_t yoe =
        (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int32_t y = static_cast<int32_t>(yoe) + static_cast<int32_t>(era) * 400;
    const uint32_t doy =
        doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    const uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    const int32_t month_index =
        static_cast<int32_t>(mp) + (mp < 10u ? 3 : -9);
    y += month_index <= 2 ? 1 : 0;

    year = static_cast<uint16_t>(y);
    month = static_cast<uint8_t>(month_index);
    day = static_cast<uint8_t>(d);
}

bool sample_to_unix_seconds(const RtcSample& sample, uint64_t& unix_seconds) {
    int32_t full_year = 0;
    if (sample.century != 0) {
        full_year = static_cast<int32_t>(sample.century) * 100 +
                    static_cast<int32_t>(sample.year);
    } else {
        full_year = sample.year >= 70 ? 1900 + static_cast<int32_t>(sample.year)
                                      : 2000 + static_cast<int32_t>(sample.year);
    }

    const int64_t days =
        days_from_civil(full_year, sample.month, sample.day);
    if (days < 0) {
        return false;
    }

    unix_seconds = static_cast<uint64_t>(days) * 86400ull +
                   static_cast<uint64_t>(sample.hour) * 3600ull +
                   static_cast<uint64_t>(sample.minute) * 60ull +
                   static_cast<uint64_t>(sample.second);
    return true;
}

bool read_rtc_unix_time(uint64_t& unix_seconds) {
    uint8_t century_register = rtc_century_register();
    for (uint32_t attempt = 0; attempt < 8u; ++attempt) {
        wait_for_rtc_update();
        RtcSample first = read_rtc_sample(century_register);
        wait_for_rtc_update();
        RtcSample second = read_rtc_sample(century_register);
        if (!samples_match(first, second)) {
            continue;
        }
        if (!normalize_sample(second)) {
            return false;
        }
        return sample_to_unix_seconds(second, unix_seconds);
    }
    return false;
}

void begin_write() {
    __atomic_fetch_add(&g_time_seq, 1u, __ATOMIC_RELEASE);
}

void end_write() {
    __atomic_fetch_add(&g_time_seq, 1u, __ATOMIC_RELEASE);
}

}  // namespace

namespace timekeeping {

bool init_from_rtc(uint32_t pit_frequency_hz) {
    if (pit_frequency_hz == 0) {
        pit_frequency_hz = 100;
    }

    // The monotonic clock drives scheduler deadlines and firmware timeouts,
    // so it must remain available even when the wall-clock RTC is invalid.
    begin_write();
    g_tick_hz = pit_frequency_hz;
    g_tick_remainder = 0;
    g_tick_nanos_floor =
        static_cast<uint32_t>(kNanosecondsPerSecond / pit_frequency_hz);
    g_tick_nanos_remainder =
        static_cast<uint32_t>(kNanosecondsPerSecond % pit_frequency_hz);
    g_tick_count = 0;
    g_uptime_nanoseconds = 0;
    g_tsc_frequency_hz = detect_tsc_frequency();
    g_tsc_base = g_tsc_frequency_hz != 0 ? read_tsc() : 0;
    g_last_monotonic_nanoseconds = 0;
    end_write();

    if (g_tsc_frequency_hz != 0) {
        log_message(LogLevel::Info,
                    "Time: monotonic TSC frequency %llu Hz",
                    static_cast<unsigned long long>(g_tsc_frequency_hz));
    } else {
        log_message(LogLevel::Warn,
                    "Time: TSC unavailable, using PIT-resolution monotonic clock");
    }

    uint64_t unix_seconds = 0;
    if (!read_rtc_unix_time(unix_seconds)) {
        log_message(LogLevel::Warn, "Time: failed to read CMOS RTC");
        return false;
    }

    begin_write();
    g_unix_seconds = unix_seconds;
    g_nanoseconds = 0;
    end_write();
    __atomic_store_n(&g_initialized, true, __ATOMIC_RELEASE);

    log_message(LogLevel::Info,
                "Time: initialized from CMOS RTC at %llu",
                static_cast<unsigned long long>(unix_seconds));
    return true;
}

void tick_pit() {
    begin_write();
    uint32_t remainder = g_tick_remainder + g_tick_nanos_remainder;
    bool carried = false;
    if (remainder >= g_tick_hz) {
        remainder -= g_tick_hz;
        carried = true;
    }
    if (clock_available()) {
        uint64_t nanoseconds =
            static_cast<uint64_t>(g_nanoseconds) + g_tick_nanos_floor;
        if (carried) {
            ++nanoseconds;
        }
        while (nanoseconds >= kNanosecondsPerSecond) {
            nanoseconds -= kNanosecondsPerSecond;
            ++g_unix_seconds;
        }
        g_nanoseconds = static_cast<uint32_t>(nanoseconds);
    }
    g_tick_remainder = remainder;
    ++g_tick_count;
    g_uptime_nanoseconds += g_tick_nanos_floor;
    if (carried) ++g_uptime_nanoseconds;
    end_write();
}

bool snapshot(NeutrinoWallTime& out_time) {
    if (!clock_available()) {
        log_message(
            LogLevel::Warn,
            "Time: snapshot unavailable initialized=0 unix=%llu seq=%u ticks=%llu uptime_ns=%llu",
            static_cast<unsigned long long>(
                __atomic_load_n(&g_unix_seconds, __ATOMIC_RELAXED)),
            static_cast<unsigned int>(
                __atomic_load_n(&g_time_seq, __ATOMIC_RELAXED)),
            static_cast<unsigned long long>(
                __atomic_load_n(&g_tick_count, __ATOMIC_RELAXED)),
            static_cast<unsigned long long>(
                __atomic_load_n(&g_uptime_nanoseconds, __ATOMIC_RELAXED)));
        return false;
    }

    uint64_t unix_seconds = 0;
    uint32_t nanoseconds = 0;
    for (;;) {
        uint32_t seq_before = __atomic_load_n(&g_time_seq, __ATOMIC_ACQUIRE);
        if ((seq_before & 1u) != 0) {
            continue;
        }
        unix_seconds = __atomic_load_n(&g_unix_seconds, __ATOMIC_RELAXED);
        nanoseconds = __atomic_load_n(&g_nanoseconds, __ATOMIC_RELAXED);
        uint32_t seq_after = __atomic_load_n(&g_time_seq, __ATOMIC_ACQUIRE);
        if (seq_before == seq_after) {
            break;
        }
    }

    const uint64_t seconds_of_day = unix_seconds % 86400ull;
    const int64_t days = static_cast<int64_t>(unix_seconds / 86400ull);

    out_time.unix_seconds = unix_seconds;
    out_time.nanoseconds = nanoseconds;
    civil_from_days(days, out_time.year, out_time.month, out_time.day);
    out_time.hour = static_cast<uint8_t>(seconds_of_day / 3600ull);
    out_time.minute =
        static_cast<uint8_t>((seconds_of_day % 3600ull) / 60ull);
    out_time.second = static_cast<uint8_t>(seconds_of_day % 60ull);
    out_time.weekday =
        static_cast<uint8_t>((days + 4ll + 7ll) % 7ll);
    out_time.reserved[0] = 0;
    out_time.reserved[1] = 0;
    out_time.reserved[2] = 0;
    return true;
}

uint64_t tick_count() {
    uint64_t ticks = 0;
    for (;;) {
        uint32_t seq_before = __atomic_load_n(&g_time_seq, __ATOMIC_ACQUIRE);
        if ((seq_before & 1u) != 0) {
            continue;
        }
        ticks = __atomic_load_n(&g_tick_count, __ATOMIC_RELAXED);
        uint32_t seq_after = __atomic_load_n(&g_time_seq, __ATOMIC_ACQUIRE);
        if (seq_before == seq_after) {
            return ticks;
        }
    }
}

uint64_t nanoseconds_since_boot() {
    uint64_t candidate = 0;
    const uint64_t frequency_hz =
        __atomic_load_n(&g_tsc_frequency_hz, __ATOMIC_ACQUIRE);
    if (frequency_hz != 0) {
        const uint64_t elapsed_cycles =
            read_tsc() - __atomic_load_n(&g_tsc_base, __ATOMIC_RELAXED);
        const uint64_t whole_seconds = elapsed_cycles / frequency_hz;
        const uint64_t remaining_cycles = elapsed_cycles % frequency_hz;
        if (whole_seconds <= UINT64_MAX / kNanosecondsPerSecond) {
            candidate = whole_seconds * kNanosecondsPerSecond +
                        (remaining_cycles * kNanosecondsPerSecond) /
                        frequency_hz;
        } else {
            candidate = UINT64_MAX;
        }
    } else {
        candidate =
            __atomic_load_n(&g_uptime_nanoseconds, __ATOMIC_ACQUIRE);
    }

    // The uACPI contract requires a strictly monotonic value. Keep that
    // promise across sub-nanosecond TSC reads, unsynchronized CPUs, and the
    // coarse PIT fallback used when no invariant TSC frequency is available.
    uint64_t previous =
        __atomic_load_n(&g_last_monotonic_nanoseconds, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t next = candidate > previous ? candidate : previous + 1;
        if (previous == UINT64_MAX) return UINT64_MAX;
        if (__atomic_compare_exchange_n(&g_last_monotonic_nanoseconds,
                                        &previous, next, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            return next;
        }
    }
}

uint64_t ticks_for_duration_ns(uint64_t duration_ns) {
    if (duration_ns == 0) {
        return 0;
    }

    uint32_t tick_hz = 0;
    for (;;) {
        uint32_t seq_before = __atomic_load_n(&g_time_seq, __ATOMIC_ACQUIRE);
        if ((seq_before & 1u) != 0) {
            continue;
        }
        tick_hz = __atomic_load_n(&g_tick_hz, __ATOMIC_RELAXED);
        uint32_t seq_after = __atomic_load_n(&g_time_seq, __ATOMIC_ACQUIRE);
        if (seq_before == seq_after) {
            break;
        }
    }

    if (tick_hz == 0) {
        return 0;
    }

    uint64_t whole_seconds = duration_ns / kNanosecondsPerSecond;
    uint64_t remainder_ns = duration_ns % kNanosecondsPerSecond;
    if (whole_seconds >
        UINT64_MAX / static_cast<uint64_t>(tick_hz)) {
        return UINT64_MAX;
    }
    uint64_t ticks = whole_seconds * static_cast<uint64_t>(tick_hz);
    if (remainder_ns != 0) {
        ticks += (remainder_ns * static_cast<uint64_t>(tick_hz) +
                  kNanosecondsPerSecond - 1u) /
                 kNanosecondsPerSecond;
    }
    return ticks == 0 ? 1 : ticks;
}

}  // namespace timekeeping
