#include "path_util.hpp"

#include "fs/vfs.hpp"
#include "lib/mem.hpp"

namespace path_util {

namespace {

struct Segment {
    const char* data;
    size_t length;
};

constexpr size_t kMaxSegments = 64;

bool push_segment(Segment (&segments)[kMaxSegments],
                  size_t& count,
                  const char* start,
                  size_t length) {
    if (length == 0) {
        return true;
    }
    if (count >= kMaxSegments) {
        return false;
    }
    segments[count++] = Segment{start, length};
    return true;
}

void pop_segment(size_t& count) {
    if (count > 0) {
        --count;
    }
}

bool parse_into_segments(const char* path,
                         bool path_is_absolute,
                         size_t floor_count,
                         Segment (&segments)[kMaxSegments],
                         size_t& count) {
    if (path == nullptr) {
        return true;
    }

    const char* cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        size_t len = static_cast<size_t>(cursor - start);
        if (len == 0) {
            continue;
        }
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (count > floor_count) {
                pop_segment(count);
            } else if (!path_is_absolute) {
                // For relative paths we do not allow traversing above the root
                // of the combined base path, so ignore extra ".." segments.
            }
            continue;
        }
        if (!push_segment(segments, count, start, len)) {
            return false;
        }
    }
    return true;
}

bool write_segments(const Segment (&segments)[kMaxSegments],
                    size_t count,
                    char (&out)[kMaxPathLength]) {
    size_t length = 0;
    out[length++] = '/';

    for (size_t i = 0; i < count; ++i) {
        if (length > 1) {
            if (length + 1 >= kMaxPathLength) {
                return false;
            }
            out[length++] = '/';
        }
        if (length + segments[i].length >= kMaxPathLength) {
            return false;
        }
        memcpy(out + length, segments[i].data, segments[i].length);
        length += segments[i].length;
    }

    if (length > 1 && out[length - 1] == '/') {
        --length;
    }
    out[length] = '\0';
    return true;
}

size_t string_length(const char* str) {
    size_t len = 0;
    if (str == nullptr) {
        return 0;
    }
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

bool system_namespace_remainder(const char* path, const char*& remainder) {
    if (path == nullptr) {
        return false;
    }

    const char* cursor = path;
    while (*cursor == '/') {
        ++cursor;
    }

    constexpr char kSystemNamespace[] = "@sys";
    for (size_t i = 0; i < sizeof(kSystemNamespace) - 1; ++i) {
        if (cursor[i] == '\0' || cursor[i] != kSystemNamespace[i]) {
            return false;
        }
    }

    cursor += sizeof(kSystemNamespace) - 1;
    if (*cursor != '\0' && *cursor != '/') {
        return false;
    }
    while (*cursor == '/') {
        ++cursor;
    }
    remainder = cursor;
    return true;
}

bool volume_namespace_parts(const char* path,
                            const char*& alias,
                            size_t& alias_length,
                            const char*& remainder) {
    if (path == nullptr || path[0] != '@') return false;
    alias = path + 1;
    const char* cursor = alias;
    while (*cursor != '\0' && *cursor != '/') ++cursor;
    alias_length = static_cast<size_t>(cursor - alias);
    if (alias_length == 0) return false;
    while (*cursor == '/') ++cursor;
    remainder = cursor;
    return true;
}

}  // namespace

bool build_absolute_path(const char* base,
                         const char* input,
                         char (&out)[kMaxPathLength]) {
    Segment segments[kMaxSegments];
    size_t segment_count = 0;
    Segment mount_root_segment{nullptr, 0};

    const char* effective_base = (base != nullptr && base[0] != '\0')
                                     ? base
                                     : "/";
    size_t floor_count = vfs::has_explicit_mount_prefix(effective_base) ? 1 : 0;
    if (!parse_into_segments(effective_base,
                             true,
                             0,
                             segments,
                             segment_count)) {
        return false;
    }
    const char* root_mount = vfs::root_mount_name();
    if (root_mount != nullptr && root_mount[0] != '\0') {
        mount_root_segment = Segment{root_mount, string_length(root_mount)};
    }

    if (input == nullptr || input[0] == '\0') {
        return write_segments(segments, segment_count, out);
    }

    // @sys is an OS namespace resolved to the configured system root mount.
    // Keep the mount segment as the traversal floor so @sys/.. cannot escape
    // into the VFS mount namespace.
    const char* namespace_remainder = nullptr;
    if (system_namespace_remainder(input, namespace_remainder)) {
        if (mount_root_segment.data == nullptr ||
            mount_root_segment.length == 0) {
            return false;
        }
        segments[0] = mount_root_segment;
        segment_count = 1;
        if (!parse_into_segments(namespace_remainder,
                                 true,
                                 1,
                                 segments,
                                 segment_count)) {
            return false;
        }
        return write_segments(segments, segment_count, out);
    }

    const char* alias = nullptr;
    size_t alias_length = 0;
    if (volume_namespace_parts(input, alias, alias_length, namespace_remainder)) {
        const char* mount_name = vfs::mount_name_for_alias(alias, alias_length);
        if (mount_name == nullptr) return false;
        segments[0] = Segment{mount_name, string_length(mount_name)};
        segment_count = 1;
        if (!parse_into_segments(namespace_remainder, true, 1,
                                 segments, segment_count)) {
            return false;
        }
        return write_segments(segments, segment_count, out);
    }

    if (input[0] == '/') {
        segment_count = 0;
        if (!parse_into_segments(input, true, 0, segments, segment_count)) {
            return false;
        }
        return write_segments(segments, segment_count, out);
    }

    if (!parse_into_segments(input,
                             false,
                             floor_count,
                             segments,
                             segment_count)) {
        return false;
    }
    return write_segments(segments, segment_count, out);
}

bool build_user_path(const char* canonical,
                     char (&out)[kMaxPathLength]) {
    if (canonical == nullptr || canonical[0] != '/') {
        return false;
    }

    size_t canonical_length = string_length(canonical);
    if (canonical_length == 1) {
        out[0] = '/';
        out[1] = '\0';
        return true;
    }

    const char* root_mount = vfs::root_mount_name();
    if (root_mount == nullptr || root_mount[0] == '\0') {
        if (canonical_length >= kMaxPathLength) {
            return false;
        }
        memcpy(out, canonical, canonical_length + 1);
        return true;
    }

    const char* first = canonical + 1;
    const char* first_end = first;
    while (*first_end != '\0' && *first_end != '/') {
        ++first_end;
    }
    size_t first_length = static_cast<size_t>(first_end - first);
    size_t root_length = string_length(root_mount);
    bool explicit_system_mount =
        first_length == root_length &&
        memcmp(first, root_mount, root_length) == 0;

    if (!explicit_system_mount && vfs::has_explicit_mount_prefix(canonical)) {
        const char* alias = vfs::volume_alias_for_mount(first, first_length);
        if (alias != nullptr) {
            size_t alias_length = string_length(alias);
            if (alias_length + 1 >= kMaxPathLength) return false;
            out[0] = '@';
            memcpy(out + 1, alias, alias_length);
            size_t length = alias_length + 1;
            const char* remainder = first_end;
            while (*remainder == '/') ++remainder;
            if (*remainder != '\0') {
                size_t remainder_length = string_length(remainder);
                if (length + 1 + remainder_length >= kMaxPathLength) return false;
                out[length++] = '/';
                memcpy(out + length, remainder, remainder_length);
                length += remainder_length;
            }
            out[length] = '\0';
            return true;
        }
        if (canonical_length >= kMaxPathLength) {
            return false;
        }
        memcpy(out, canonical, canonical_length + 1);
        return true;
    }

    constexpr char kSystemNamespace[] = "@sys";
    size_t length = sizeof(kSystemNamespace) - 1;
    memcpy(out, kSystemNamespace, length);

    const char* remainder = explicit_system_mount ? first_end : canonical;
    while (*remainder == '/') {
        ++remainder;
    }
    if (*remainder != '\0') {
        size_t remainder_length = string_length(remainder);
        if (length + 1 + remainder_length >= kMaxPathLength) {
            return false;
        }
        out[length++] = '/';
        memcpy(out + length, remainder, remainder_length);
        length += remainder_length;
    }
    out[length] = '\0';
    return true;
}

}  // namespace path_util
