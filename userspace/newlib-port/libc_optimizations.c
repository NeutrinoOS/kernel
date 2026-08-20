/*
 * x86-64 implementations selected ahead of Newlib's generic libc.a members.
 *
 * Newlib's portable memcpy is deliberately conservative and its qsort is
 * tuned for a wide range of 32-bit targets.  Neutrino's Newlib ABI is x86-64,
 * so use the architecture's string instruction and a bounded introsort here.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

void* memcpy(void* restrict destination, const void* restrict source,
             size_t count) {
    void* result = destination;
    if (count != 0) {
        __asm__ volatile("cld\n\trep movsb"
                         : "+D"(destination), "+S"(source), "+c"(count)
                         :
                         : "memory");
    }
    return result;
}

void* memmove(void* destination, const void* source, size_t count) {
    void* result = destination;
    if (count == 0 || destination == source) return result;

    if ((uintptr_t)destination < (uintptr_t)source) {
        return memcpy(destination, source, count);
    }

    unsigned char* dst = (unsigned char*)destination + count - 1;
    const unsigned char* src = (const unsigned char*)source + count - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld"
                     : "+D"(dst), "+S"(src), "+c"(count)
                     :
                     : "memory");
    return result;
}

typedef int compare_function(const void*, const void*);
typedef uint64_t alias_uint64_t __attribute__((__may_alias__));
typedef uint32_t alias_uint32_t __attribute__((__may_alias__));

static inline void byte_swap(unsigned char* left, unsigned char* right,
                             size_t size) {
    while (size-- != 0) {
        unsigned char value = *left;
        *left++ = *right;
        *right++ = value;
    }
}

static inline void swap_element(unsigned char* left, unsigned char* right,
                                size_t size) {
    if (left == right) return;
    if (size == sizeof(uint64_t) && (((uintptr_t)left | (uintptr_t)right) & 7) == 0) {
        alias_uint64_t value = *(alias_uint64_t*)left;
        *(alias_uint64_t*)left = *(alias_uint64_t*)right;
        *(alias_uint64_t*)right = value;
        return;
    }
    if (size == sizeof(uint32_t) && (((uintptr_t)left | (uintptr_t)right) & 3) == 0) {
        alias_uint32_t value = *(alias_uint32_t*)left;
        *(alias_uint32_t*)left = *(alias_uint32_t*)right;
        *(alias_uint32_t*)right = value;
        return;
    }
    byte_swap(left, right, size);
}

static void insertion_sort(unsigned char* base, size_t count, size_t size,
                           compare_function* compare) {
    for (size_t index = 1; index < count; ++index) {
        for (size_t current = index; current != 0; --current) {
            unsigned char* right = base + current * size;
            unsigned char* left = right - size;
            if (compare(left, right) <= 0) break;
            swap_element(left, right, size);
        }
    }
}

static void sift_down(unsigned char* base, size_t root, size_t count,
                      size_t size, compare_function* compare) {
    for (;;) {
        size_t child = root * 2 + 1;
        if (child >= count) return;
        if (child + 1 < count &&
            compare(base + child * size, base + (child + 1) * size) < 0) {
            ++child;
        }
        if (compare(base + root * size, base + child * size) >= 0) return;
        swap_element(base + root * size, base + child * size, size);
        root = child;
    }
}

static void heap_sort(unsigned char* base, size_t count, size_t size,
                      compare_function* compare) {
    for (size_t root = count / 2; root != 0; --root)
        sift_down(base, root - 1, count, size, compare);
    for (size_t tail = count; tail > 1; --tail) {
        swap_element(base, base + (tail - 1) * size, size);
        sift_down(base, 0, tail - 1, size, compare);
    }
}

void qsort(void* base, size_t count, size_t size, compare_function* compare) {
    if (count < 2 || size == 0 || compare == NULL) return;

    struct range { unsigned char* base; size_t count; unsigned depth; };
    struct range pending[sizeof(size_t) * 8];
    unsigned pending_count = 0;
    unsigned depth = 0;
    for (size_t value = count; value > 1; value >>= 1) ++depth;
    depth *= 2;

    unsigned char* current_base = base;
    size_t current_count = count;
    for (;;) {
        while (current_count > 16) {
            if (depth == 0) {
                heap_sort(current_base, current_count, size, compare);
                goto next_range;
            }
            --depth;

            unsigned char* first = current_base;
            unsigned char* middle = current_base + (current_count / 2) * size;
            unsigned char* last = current_base + (current_count - 1) * size;
            if (compare(first, middle) > 0) swap_element(first, middle, size);
            if (compare(middle, last) > 0) swap_element(middle, last, size);
            if (compare(first, middle) > 0) swap_element(first, middle, size);
            swap_element(first, middle, size);

            size_t less = 1;
            size_t scan = 1;
            size_t greater = current_count;
            while (scan < greater) {
                unsigned char* element = current_base + scan * size;
                int order = compare(element, first);
                if (order < 0) {
                    swap_element(element, current_base + less * size, size);
                    ++less;
                    ++scan;
                } else if (order > 0) {
                    --greater;
                    swap_element(element, current_base + greater * size, size);
                } else {
                    ++scan;
                }
            }
            swap_element(first, current_base + (less - 1) * size, size);

            size_t left_count = less - 1;
            size_t right_count = current_count - greater;
            if (left_count < right_count) {
                if (right_count > 1)
                    pending[pending_count++] = (struct range){
                        current_base + greater * size, right_count, depth};
                current_count = left_count;
            } else {
                if (left_count > 1)
                    pending[pending_count++] = (struct range){
                        current_base, left_count, depth};
                current_base += greater * size;
                current_count = right_count;
            }
        }
        if (current_count > 1)
            insertion_sort(current_base, current_count, size, compare);

next_range:
        if (pending_count == 0) return;
        struct range next = pending[--pending_count];
        current_base = next.base;
        current_count = next.count;
        depth = next.depth;
    }
}
