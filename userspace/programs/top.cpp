#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../libc/include/neutrino.h"

namespace {

constexpr size_t kMaxCpuEntries = 16;
constexpr size_t kMaxTaskEntries = 256;
constexpr size_t kMaxVisibleTasks = 12;
constexpr size_t kMaxRenderRows = 64;
constexpr uint64_t kExpectedTicksPerSecond = 100;
constexpr uint64_t kRefreshSeconds = 5;
constexpr uint32_t kDefaultCols = 80;
constexpr uint32_t kDefaultRows = 25;
constexpr uint32_t kTaskStateUnused = 0;
constexpr uint32_t kTaskStateReady = 1;
constexpr uint32_t kTaskStateRunning = 2;
constexpr uint32_t kTaskStateBlocked = 3;
constexpr uint32_t kTaskStateTerminated = 4;
constexpr uint32_t kTaskStateSuspending = 8;
constexpr uint32_t kTaskStateSuspended = 9;

struct TaskDelta {
    descriptor_defs::TaskUsage snapshot;
    uint64_t delta_ticks;
};

descriptor_defs::CpuUsage g_previous_cpus[kMaxCpuEntries]{};
descriptor_defs::CpuUsage g_current_cpus[kMaxCpuEntries]{};
descriptor_defs::TaskUsage g_previous_tasks[kMaxTaskEntries]{};
descriptor_defs::TaskUsage g_current_tasks[kMaxTaskEntries]{};
TaskDelta g_deltas[kMaxTaskEntries]{};
char g_render_buffer[8192]{};
uint32_t g_previous_line_widths[kMaxRenderRows]{};
uint32_t g_current_line_widths[kMaxRenderRows]{};

void append_char(char* buffer, size_t capacity, size_t& length, char ch) {
    if (length + 1 >= capacity) {
        return;
    }
    buffer[length++] = ch;
    buffer[length] = '\0';
}

void append_text(char* buffer, size_t capacity, size_t& length, const char* text) {
    if (text == nullptr) {
        return;
    }
    while (*text != '\0' && length + 1 < capacity) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
}

void append_u64(char* buffer, size_t capacity, size_t& length, uint64_t value) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));

    while (count > 0) {
        append_char(buffer, capacity, length, digits[--count]);
    }
}

void append_percent_tenths(char* buffer,
                           size_t capacity,
                           size_t& length,
                           uint64_t part,
                           uint64_t total) {
    if (total == 0) {
        append_text(buffer, capacity, length, "0.0");
        return;
    }
    uint64_t scaled = (part * 1000ull) / total;
    append_u64(buffer, capacity, length, scaled / 10ull);
    append_char(buffer, capacity, length, '.');
    append_char(buffer,
                capacity,
                length,
                static_cast<char>('0' + (scaled % 10ull)));
}

void append_percent_tenths_padded(char* buffer,
                                  size_t capacity,
                                  size_t& length,
                                  uint64_t part,
                                  uint64_t total,
                                  size_t width) {
    char percent[16];
    size_t percent_length = 0;
    percent[0] = '\0';
    append_percent_tenths(percent, sizeof(percent), percent_length, part, total);

    size_t padding = width > percent_length ? width - percent_length : 0;
    while (padding > 0) {
        append_char(buffer, capacity, length, ' ');
        --padding;
    }
    append_text(buffer, capacity, length, percent);
}

void append_padded_text(char* buffer,
                        size_t capacity,
                        size_t& length,
                        const char* text,
                        size_t width) {
    size_t written = 0;
    while (text != nullptr && text[written] != '\0' && written < width) {
        append_char(buffer, capacity, length, text[written]);
        ++written;
    }
    while (written < width) {
        append_char(buffer, capacity, length, ' ');
        ++written;
    }
}

void append_padded_u64(char* buffer,
                       size_t capacity,
                       size_t& length,
                       uint64_t value,
                       size_t width) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));

    size_t padding = width > count ? width - count : 0;
    while (padding > 0) {
        append_char(buffer, capacity, length, ' ');
        --padding;
    }
    while (count > 0) {
        append_char(buffer, capacity, length, digits[--count]);
    }
}

const char* task_state_name(uint32_t state) {
    switch (state) {
        case kTaskStateReady:
            return "ready";
        case kTaskStateRunning:
            return "run";
        case kTaskStateBlocked:
            return "block";
        case kTaskStateTerminated:
            return "term";
        case kTaskStateSuspending:
            return "stop?";
        case kTaskStateSuspended:
            return "stop";
        case kTaskStateUnused:
        default:
            return "unused";
    }
}

long open_monitor_descriptor(descriptor_defs::Type type) {
    return descriptor_open(static_cast<uint32_t>(type), 0, 0, 0);
}

bool clear_screen(long console) {
    if (console < 0) {
        return false;
    }
    long type = descriptor_get_type(static_cast<uint32_t>(console));
    if (type == static_cast<long>(descriptor_defs::Type::Console)) {
        return descriptor_set_property(
                   static_cast<uint32_t>(console),
                   static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
                   nullptr,
                   0) == 0;
    }
    if (type == static_cast<long>(descriptor_defs::Type::Vty)) {
        return descriptor_set_property(
                   static_cast<uint32_t>(console),
                   static_cast<uint32_t>(descriptor_defs::Property::VtyClear),
                   nullptr,
                   0) == 0;
    }
    return false;
}

bool set_cursor(long console, uint32_t x, uint32_t y) {
    descriptor_defs::CursorPosition pos{x, y};
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor),
               &pos,
               sizeof(pos)) == 0;
}

void defer_console_updates(long console, bool deferred) {
    uint8_t value = deferred ? 1 : 0;
    descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleUpdate),
        &value,
        sizeof(value));
}

void console_dimensions(long console, uint32_t& cols, uint32_t& rows) {
    cols = kDefaultCols;
    rows = kDefaultRows;
    descriptor_defs::VtyInfo info{};
    if (descriptor_get_property(
            static_cast<uint32_t>(console),
            static_cast<uint32_t>(descriptor_defs::Property::VtyInfo),
            &info,
            sizeof(info)) == 0) {
        if (info.cols != 0) {
            cols = info.cols;
        }
        if (info.rows != 0) {
            rows = info.rows;
        }
    }
}

size_t read_cpu_stats(uint32_t handle,
                      descriptor_defs::CpuUsage* out,
                      size_t capacity) {
    long result = descriptor_read(handle,
                                  out,
                                  capacity * sizeof(descriptor_defs::CpuUsage),
                                  0);
    if (result <= 0) {
        return 0;
    }
    return static_cast<size_t>(result) / sizeof(descriptor_defs::CpuUsage);
}

size_t read_task_stats(uint32_t handle,
                       descriptor_defs::TaskUsage* out,
                       size_t capacity) {
    long result = descriptor_read(handle,
                                  out,
                                  capacity * sizeof(descriptor_defs::TaskUsage),
                                  0);
    if (result <= 0) {
        return 0;
    }
    return static_cast<size_t>(result) / sizeof(descriptor_defs::TaskUsage);
}

uint64_t delta_u64(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : current;
}

uint64_t cpu_usage_total(const descriptor_defs::CpuUsage& usage) {
    return usage.user_ticks + usage.kernel_ticks + usage.idle_ticks +
           usage.irq_ticks;
}

uint64_t cpu_usage_delta(const descriptor_defs::CpuUsage& current,
                         const descriptor_defs::CpuUsage& previous) {
    return delta_u64(cpu_usage_total(current), cpu_usage_total(previous));
}

const descriptor_defs::TaskUsage* find_previous_task(
    const descriptor_defs::TaskUsage* previous,
    size_t previous_count,
    uint32_t pid) {
    for (size_t i = 0; i < previous_count; ++i) {
        if (previous[i].pid == pid) {
            return &previous[i];
        }
    }
    return nullptr;
}

size_t build_task_deltas(TaskDelta* out,
                         size_t capacity,
                         const descriptor_defs::TaskUsage* current,
                         size_t current_count,
                         const descriptor_defs::TaskUsage* previous,
                         size_t previous_count) {
    size_t count = 0;
    for (size_t i = 0; i < current_count && count < capacity; ++i) {
        if (current[i].state == kTaskStateUnused) {
            continue;
        }
        out[count].snapshot = current[i];
        const descriptor_defs::TaskUsage* prev =
            find_previous_task(previous, previous_count, current[i].pid);
        uint64_t prev_total = 0;
        if (prev != nullptr) {
            prev_total = prev->user_ticks + prev->kernel_ticks;
        }
        uint64_t total = current[i].user_ticks + current[i].kernel_ticks;
        out[count].delta_ticks = total >= prev_total ? total - prev_total : total;
        ++count;
    }
    return count;
}

void sort_task_deltas(TaskDelta* deltas, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < count; ++j) {
            if (deltas[j].delta_ticks > deltas[best].delta_ticks) {
                best = j;
            }
        }
        if (best == i) {
            continue;
        }
        TaskDelta tmp = deltas[i];
        deltas[i] = deltas[best];
        deltas[best] = tmp;
    }
}

uint64_t total_cpu_delta(const descriptor_defs::CpuUsage* current,
                         const descriptor_defs::CpuUsage* previous,
                         size_t count) {
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += cpu_usage_delta(current[i], previous[i]);
    }
    return total;
}

uint64_t display_cpu_total(uint64_t sampled_total) {
    uint64_t expected = kExpectedTicksPerSecond * kRefreshSeconds;
    return sampled_total < expected ? expected : sampled_total;
}

uint64_t display_interval_total(uint64_t sampled_total, size_t cpu_count) {
    uint64_t expected = kExpectedTicksPerSecond * kRefreshSeconds *
                        static_cast<uint64_t>(cpu_count == 0 ? 1 : cpu_count);
    return sampled_total < expected ? expected : sampled_total;
}

void wait_for_next_sample() {
    sleep_seconds(kRefreshSeconds);
}

void finish_line(char* buffer,
                 size_t capacity,
                 size_t& length,
                 size_t line_start,
                 uint32_t cols,
                 uint32_t row) {
    size_t used = length >= line_start ? length - line_start : 0;
    uint32_t max_width = cols > 1 ? cols - 1 : 1;
    if (used > max_width) {
        length = line_start + max_width;
        buffer[length] = '\0';
        used = max_width;
    }
    uint32_t previous_width =
        row < kMaxRenderRows ? g_previous_line_widths[row] : 0;
    uint32_t target = static_cast<uint32_t>(used);
    if (target < previous_width) {
        target = previous_width;
    }
    if (target > max_width) {
        target = max_width;
    }
    while (used < target) {
        append_char(buffer, capacity, length, ' ');
        ++used;
    }
    if (row < kMaxRenderRows) {
        g_current_line_widths[row] = static_cast<uint32_t>(length - line_start);
    }
    append_text(buffer, capacity, length, "\n");
}

void append_line(char* buffer,
                 size_t capacity,
                 size_t& length,
                 const char* text,
                 uint32_t cols,
                 uint32_t row) {
    size_t line_start = length;
    append_text(buffer, capacity, length, text);
    finish_line(buffer, capacity, length, line_start, cols, row);
}

void append_cpu_line(char* buffer,
                     size_t capacity,
                     size_t& length,
                     const descriptor_defs::CpuUsage& current,
                     const descriptor_defs::CpuUsage& previous,
                     uint32_t cols,
                     uint32_t row) {
    size_t line_start = length;
    uint64_t user = delta_u64(current.user_ticks, previous.user_ticks);
    uint64_t kernel = delta_u64(current.kernel_ticks, previous.kernel_ticks);
    uint64_t idle = delta_u64(current.idle_ticks, previous.idle_ticks);
    uint64_t irq = delta_u64(current.irq_ticks, previous.irq_ticks);
    uint64_t sampled_total = user + kernel + idle + irq;
    uint64_t total = display_cpu_total(sampled_total);
    if (sampled_total < total) {
        idle += total - sampled_total;
    }

    append_text(buffer, capacity, length, "cpu");
    append_u64(buffer, capacity, length, current.cpu_index);
    append_text(buffer, capacity, length, "  user ");
    append_percent_tenths_padded(buffer, capacity, length, user, total, 5);
    append_text(buffer, capacity, length, "%  kernel ");
    append_percent_tenths_padded(buffer, capacity, length, kernel, total, 5);
    append_text(buffer, capacity, length, "%  idle ");
    append_percent_tenths_padded(buffer, capacity, length, idle, total, 5);
    append_text(buffer, capacity, length, "%  irq ");
    append_percent_tenths_padded(buffer, capacity, length, irq, total, 5);
    append_text(buffer, capacity, length, "%");
    finish_line(buffer, capacity, length, line_start, cols, row);
}

void append_task_table(char* buffer,
                       size_t capacity,
                       size_t& length,
                       TaskDelta* deltas,
                       size_t count,
                       uint64_t interval_total,
                       uint32_t cols,
                       size_t visible_limit,
                       uint32_t first_row) {
    append_line(buffer, capacity, length, "", cols, first_row);
    append_line(buffer, capacity, length, "       pid   cpu%    rss  state  kind    image", cols, first_row + 1);
    append_line(buffer, capacity, length, "------------------------------------------------------------------", cols, first_row + 2);

    size_t visible = count < visible_limit ? count : visible_limit;
    for (size_t i = 0; i < visible; ++i) {
        uint32_t row = first_row + 3 + static_cast<uint32_t>(i);
        size_t line_start = length;
        append_padded_u64(buffer, capacity, length, deltas[i].snapshot.pid, 10);
        append_text(buffer, capacity, length, "  ");
        append_percent_tenths_padded(buffer,
                                     capacity,
                                     length,
                                     deltas[i].delta_ticks,
                                     interval_total,
                                     5);
        append_text(buffer, capacity, length, "  ");
        append_padded_u64(buffer,
                          capacity,
                          length,
                          deltas[i].snapshot.resident_bytes / 1024,
                          5);
        append_text(buffer, capacity, length, "K  ");
        append_padded_text(buffer,
                           capacity,
                           length,
                           task_state_name(deltas[i].snapshot.state),
                           5);
        append_text(buffer, capacity, length, "  ");
        append_padded_text(buffer,
                           capacity,
                           length,
                           (deltas[i].snapshot.flags &
                            descriptor_defs::kTaskStatFlagKernel)
                               ? "kernel"
                               : ((deltas[i].snapshot.flags &
                                   descriptor_defs::kTaskStatFlagThread)
                                      ? "thread"
                                      : "user"),
                           6);
        append_text(buffer, capacity, length, "  ");
        append_text(buffer, capacity, length, deltas[i].snapshot.image_path);
        finish_line(buffer, capacity, length, line_start, cols, row);
    }
}

}  // namespace

extern "C" int main(uint64_t, uint64_t) {
    long console = neutrino_open_stdout();
    if (console < 0) {
        return 1;
    }

    long cpu_desc = open_monitor_descriptor(descriptor_defs::Type::CpuStats);
    if (cpu_desc < 0) {
        neutrino_write_line(console, "top: failed to open cpu stats (missing monitor capability?)");
        return 1;
    }

    long task_desc = open_monitor_descriptor(descriptor_defs::Type::TaskStats);
    if (task_desc < 0) {
        neutrino_write_line(console, "top: failed to open task stats (missing monitor capability?)");
        descriptor_close(static_cast<uint32_t>(cpu_desc));
        return 1;
    }

    size_t previous_cpu_count =
        read_cpu_stats(static_cast<uint32_t>(cpu_desc), g_previous_cpus, kMaxCpuEntries);
    size_t previous_task_count =
        read_task_stats(static_cast<uint32_t>(task_desc), g_previous_tasks, kMaxTaskEntries);
    uint32_t previous_render_rows = 0;
    bool first_render = true;
    clear_screen(console);

    for (;;) {
        if (!first_render) {
            wait_for_next_sample();
        }

        size_t current_cpu_count =
            read_cpu_stats(static_cast<uint32_t>(cpu_desc), g_current_cpus, kMaxCpuEntries);
        size_t cpu_count = current_cpu_count < previous_cpu_count
                               ? current_cpu_count
                               : previous_cpu_count;

        size_t current_task_count =
            read_task_stats(static_cast<uint32_t>(task_desc), g_current_tasks, kMaxTaskEntries);
        size_t delta_count = build_task_deltas(g_deltas,
                                               kMaxTaskEntries,
                                               g_current_tasks,
                                               current_task_count,
                                               g_previous_tasks,
                                               previous_task_count);
        sort_task_deltas(g_deltas, delta_count);

        char* buffer = g_render_buffer;
        size_t length = 0;
        buffer[0] = '\0';
        memset(g_current_line_widths, 0, sizeof(g_current_line_widths));

        uint32_t cols = kDefaultCols;
        uint32_t rows = kDefaultRows;
        console_dimensions(console, cols, rows);

        uint32_t render_rows = 0;
        append_line(buffer, sizeof(g_render_buffer), length, "neutrino top", cols, render_rows);
        ++render_rows;
        append_line(buffer, sizeof(g_render_buffer), length, "", cols, render_rows);
        ++render_rows;

        for (size_t i = 0; i < cpu_count; ++i) {
            append_cpu_line(buffer,
                            sizeof(g_render_buffer),
                            length,
                            g_current_cpus[i],
                            g_previous_cpus[i],
                            cols,
                            render_rows);
            ++render_rows;
        }

        uint64_t interval_total =
            total_cpu_delta(g_current_cpus, g_previous_cpus, cpu_count);
        uint64_t displayed_interval_total =
            display_interval_total(interval_total, cpu_count);
        size_t fixed_rows = render_rows + 3;
        size_t visible_tasks = kMaxVisibleTasks;
        if (rows > fixed_rows) {
            size_t available = rows - fixed_rows;
            if (visible_tasks > available) {
                visible_tasks = available;
            }
        } else {
            visible_tasks = 0;
        }
        append_task_table(buffer,
                          sizeof(g_render_buffer),
                          length,
                          g_deltas,
                          delta_count,
                          displayed_interval_total == 0 ? 1 : displayed_interval_total,
                          cols,
                          visible_tasks,
                          render_rows);
        render_rows += 3 + static_cast<uint32_t>(
                                delta_count < visible_tasks ? delta_count
                                                            : visible_tasks);

        while (render_rows < previous_render_rows && render_rows < rows) {
            append_line(buffer, sizeof(g_render_buffer), length, "", cols, render_rows);
            ++render_rows;
        }

        defer_console_updates(console, true);
        set_cursor(console, 0, 0);
        neutrino_write(console, buffer);
        set_cursor(console, 0, 0);
        defer_console_updates(console, false);

        memcpy(g_previous_cpus, g_current_cpus, sizeof(g_previous_cpus));
        memcpy(g_previous_tasks, g_current_tasks, sizeof(g_previous_tasks));
        previous_cpu_count = current_cpu_count;
        previous_task_count = current_task_count;
        previous_render_rows = render_rows;
        memcpy(g_previous_line_widths,
               g_current_line_widths,
               sizeof(g_previous_line_widths));
        first_render = false;
    }
}
