#pragma once

namespace process {
struct Task;
}

namespace userspace {

[[noreturn]] void enter_task(process::Task& proc);

}  // namespace userspace
