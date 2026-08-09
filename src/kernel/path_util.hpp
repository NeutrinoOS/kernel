#pragma once

#include <stddef.h>

namespace path_util {

constexpr size_t kMaxPathLength = 128;

// Builds an absolute, canonical path by combining an existing absolute base
// path with an input path that may be absolute or relative. A leading @sys
// namespace resolves to the configured system root mount. Returns false if the
// namespace is unavailable, the resolved path exceeds kMaxPathLength, or the
// inputs are invalid.
bool build_absolute_path(const char* base,
                         const char* input,
                         char (&out)[kMaxPathLength]);

// Converts a canonical VFS path into its user-facing namespace form. Paths on
// the configured system root are returned below @sys; other explicit mounts
// retain their canonical paths.
bool build_user_path(const char* canonical,
                     char (&out)[kMaxPathLength]);

}  // namespace path_util
