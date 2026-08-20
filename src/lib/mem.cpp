#include "mem.hpp"

#include "arch/x86_64/cpu_features.hpp"

extern "C" void *memcpy(void *dest, const void *src, size_t n) {
    void* result = dest;
    if (n != 0) {
        // Neutrino is x86-64-only.  rep movsb is both architecturally
        // available and lets current CPUs select their optimized fast-string
        // path, rather than forcing every caller through a byte loop.
        asm volatile("cld\n\trep movsb"
                     : "+D"(dest), "+S"(src), "+c"(n)
                     :
                     : "memory");
    }
    return result;
}

extern "C" void *memmove(void *dest, const void *src, size_t n) {
    void* result = dest;
    if (n == 0 || dest == src) {
        return result;
    }

    if (reinterpret_cast<uintptr_t>(dest) < reinterpret_cast<uintptr_t>(src)) {
        return memcpy(dest, src, n);
    }

    auto* d = static_cast<uint8_t*>(dest) + n - 1;
    auto* s = static_cast<const uint8_t*>(src) + n - 1;
    asm volatile("std\n\trep movsb\n\tcld"
                 : "+D"(d), "+S"(s), "+c"(n)
                 :
                 : "memory");
    return result;
}

extern "C" void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)c;
    return s;
}

extern "C" int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t*)s1;
    const uint8_t *b = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

namespace {

constexpr size_t kWordSize = sizeof(uint64_t);
constexpr size_t kSimdBlockSize = 64;
constexpr size_t kSimdThreshold = 1024;
constexpr size_t kSimdChunkSize = 16 * 1024;

inline void copy_forward_align(uint8_t*& dst,
                               const uint8_t*& src,
                               size_t& remaining) {
    while (remaining != 0 &&
           (reinterpret_cast<uintptr_t>(dst) & (kWordSize - 1)) != 0) {
        *dst++ = *src++;
        --remaining;
    }
}

inline void copy_backward_align(uint8_t*& dst,
                                const uint8_t*& src,
                                size_t& remaining) {
    while (remaining != 0 &&
           (reinterpret_cast<uintptr_t>(dst) & (kWordSize - 1)) != 0) {
        --dst;
        --src;
        *dst = *src;
        --remaining;
    }
}

inline void copy_sse2_blocks(uint8_t*& dst,
                             const uint8_t*& src,
                             size_t block_count) {
    asm volatile(
        "1:\n"
        "movdqu 0(%[src]), %%xmm0\n"
        "movdqu 16(%[src]), %%xmm1\n"
        "movdqu 32(%[src]), %%xmm2\n"
        "movdqu 48(%[src]), %%xmm3\n"
        "movdqu %%xmm0, 0(%[dst])\n"
        "movdqu %%xmm1, 16(%[dst])\n"
        "movdqu %%xmm2, 32(%[dst])\n"
        "movdqu %%xmm3, 48(%[dst])\n"
        "add $64, %[src]\n"
        "add $64, %[dst]\n"
        "dec %[count]\n"
        "jnz 1b\n"
        : [dst] "+r"(dst), [src] "+r"(src), [count] "+r"(block_count)
        :
        : "memory");
}

}  // namespace

extern "C" void *memcpy_fast(void *dest, const void *src, size_t n) {
    if (n == 0 || dest == src) {
        return dest;
    }

    if (n < 32) {
        return memcpy(dest, src, n);
    }

    auto* d = static_cast<uint8_t*>(dest);
    auto* s = static_cast<const uint8_t*>(src);
    size_t remaining = n;

    copy_forward_align(d, s, remaining);

    while (remaining >= kWordSize * 4) {
        auto* dst64 = reinterpret_cast<uint64_t*>(d);
        auto* src64 = reinterpret_cast<const uint64_t*>(s);
        dst64[0] = src64[0];
        dst64[1] = src64[1];
        dst64[2] = src64[2];
        dst64[3] = src64[3];
        d += kWordSize * 4;
        s += kWordSize * 4;
        remaining -= kWordSize * 4;
    }

    while (remaining >= kWordSize) {
        *reinterpret_cast<uint64_t*>(d) = *reinterpret_cast<const uint64_t*>(s);
        d += kWordSize;
        s += kWordSize;
        remaining -= kWordSize;
    }

    while (remaining != 0) {
        *d++ = *s++;
        --remaining;
    }

    return dest;
}

extern "C" void *memmove_fast(void *dest, const void *src, size_t n) {
    if (n == 0 || dest == src) {
        return dest;
    }

    auto* d = static_cast<uint8_t*>(dest);
    auto* s = static_cast<const uint8_t*>(src);

    if (d < s) {
        return memcpy_fast(dest, src, n);
    }

    if (n < 32) {
        return memmove(dest, src, n);
    }

    size_t remaining = n;
    auto* d_end = d + n;
    auto* s_end = s + n;

    copy_backward_align(d_end, s_end, remaining);

    while (remaining >= kWordSize * 4) {
        d_end -= kWordSize * 4;
        s_end -= kWordSize * 4;
        auto* dst64 = reinterpret_cast<uint64_t*>(d_end);
        auto* src64 = reinterpret_cast<const uint64_t*>(s_end);
        dst64[3] = src64[3];
        dst64[2] = src64[2];
        dst64[1] = src64[1];
        dst64[0] = src64[0];
        remaining -= kWordSize * 4;
    }

    while (remaining >= kWordSize) {
        d_end -= kWordSize;
        s_end -= kWordSize;
        *reinterpret_cast<uint64_t*>(d_end) =
            *reinterpret_cast<const uint64_t*>(s_end);
        remaining -= kWordSize;
    }

    while (remaining != 0) {
        --d_end;
        --s_end;
        *d_end = *s_end;
        --remaining;
    }

    return dest;
}

extern "C" void *memcpy_simd(void *dest, const void *src, size_t n) {
    if (n < kSimdThreshold || dest == src) {
        return memcpy_fast(dest, src, n);
    }

    auto* d = static_cast<uint8_t*>(dest);
    auto* s = static_cast<const uint8_t*>(src);
    size_t remaining = n;

    while (remaining >= kSimdThreshold) {
        size_t bytes = remaining;
        if (bytes > kSimdChunkSize) {
            bytes = kSimdChunkSize;
        }
        bytes &= ~(kSimdBlockSize - 1);
        if (bytes == 0 || !cpu::kernel_fpu_begin()) {
            break;
        }
        copy_sse2_blocks(d, s, bytes / kSimdBlockSize);
        cpu::kernel_fpu_end();
        remaining -= bytes;
    }

    if (remaining != 0) {
        memcpy_fast(d, s, remaining);
    }
    return dest;
}

extern "C" void *memmove_simd(void *dest, const void *src, size_t n) {
    auto* d = static_cast<uint8_t*>(dest);
    auto* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        return memcpy_simd(dest, src, n);
    }
    return memmove_fast(dest, src, n);
}
