#include "cmdline.hpp"

#include "string_util.hpp"

namespace {

constexpr size_t kStorageSize = 256;
constexpr size_t kMaxTokens = (kStorageSize + 1) / 2;

char g_raw[kStorageSize]{};
char g_parsed[kStorageSize]{};
kernel_cmdline::Token g_tokens[kMaxTokens]{};
size_t g_token_count = 0;
bool g_truncated = false;

bool is_separator(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

bool token_name_equals(const kernel_cmdline::Token& token,
                       const char* expected) {
    return expected != nullptr && string_util::equals(token.name, expected);
}

}  // namespace

namespace kernel_cmdline {

void initialize(const char* cmdline) {
    g_raw[0] = '\0';
    g_parsed[0] = '\0';
    g_token_count = 0;
    g_truncated = false;

    if (cmdline == nullptr) {
        return;
    }

    size_t length = string_util::length(cmdline);
    if (length >= kStorageSize) {
        length = kStorageSize - 1;
        g_truncated = true;
    }

    for (size_t i = 0; i < length; ++i) {
        g_raw[i] = cmdline[i];
        g_parsed[i] = cmdline[i];
    }
    g_raw[length] = '\0';
    g_parsed[length] = '\0';

    // A truncated final token may be only a prefix of the bootloader's token.
    // Do not expose that incomplete assignment or flag to consumers.
    if (g_truncated) {
        while (length > 0 && !is_separator(g_parsed[length - 1])) {
            --length;
        }
        g_parsed[length] = '\0';
    }

    char* cursor = g_parsed;
    while (*cursor != '\0') {
        while (is_separator(*cursor)) {
            *cursor++ = '\0';
        }
        if (*cursor == '\0') {
            break;
        }

        char* name = cursor;
        char* value = nullptr;
        while (*cursor != '\0' && !is_separator(*cursor)) {
            if (*cursor == '=' && value == nullptr) {
                *cursor = '\0';
                value = cursor + 1;
            }
            ++cursor;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }

        if (*name == '\0') {
            continue;
        }
        if (g_token_count < kMaxTokens) {
            g_tokens[g_token_count++] = {name, value};
        }
    }
}

const char* raw() {
    return g_raw;
}

bool truncated() {
    return g_truncated;
}

size_t token_count() {
    return g_token_count;
}

const Token* token_at(size_t index) {
    return index < g_token_count ? &g_tokens[index] : nullptr;
}

bool has_flag(const char* name) {
    for (size_t i = 0; i < g_token_count; ++i) {
        if (g_tokens[i].value == nullptr &&
            token_name_equals(g_tokens[i], name)) {
            return true;
        }
    }
    return false;
}

bool has_value(const char* name, const char* value) {
    if (value == nullptr) {
        return false;
    }
    for (size_t i = 0; i < g_token_count; ++i) {
        if (g_tokens[i].value != nullptr &&
            token_name_equals(g_tokens[i], name) &&
            string_util::equals(g_tokens[i].value, value)) {
            return true;
        }
    }
    return false;
}

size_t value_count(const char* name) {
    size_t count = 0;
    for (size_t i = 0; i < g_token_count; ++i) {
        if (g_tokens[i].value != nullptr &&
            token_name_equals(g_tokens[i], name)) {
            ++count;
        }
    }
    return count;
}

const char* value(const char* name) {
    for (size_t i = g_token_count; i > 0; --i) {
        const Token& token = g_tokens[i - 1];
        if (token.value != nullptr && token_name_equals(token, name)) {
            return token.value;
        }
    }
    return nullptr;
}

const char* value_at(const char* name, size_t index) {
    for (size_t i = 0; i < g_token_count; ++i) {
        const Token& token = g_tokens[i];
        if (token.value != nullptr && token_name_equals(token, name)) {
            if (index == 0) {
                return token.value;
            }
            --index;
        }
    }
    return nullptr;
}

}  // namespace kernel_cmdline
