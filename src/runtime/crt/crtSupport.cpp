#include <cstddef>
#include <intrin.h>

extern "C" {

int _fltused = 0;

void* __cdecl memcpy(void* dest, const void* src, size_t count);
void* __cdecl memset(void* dest, int value, size_t count);
void* __cdecl memmove(void* dest, const void* src, size_t count);
int __cdecl memcmp(const void* lhs, const void* rhs, size_t count);

#pragma function(memcpy, memset, memmove, memcmp)

void* __cdecl memcpy(void* dest, const void* src, size_t count)
{
    __movsb(static_cast<unsigned char*>(dest), static_cast<const unsigned char*>(src), count);
    return dest;
}

void* __cdecl memset(void* dest, int value, size_t count)
{
    __stosb(static_cast<unsigned char*>(dest), static_cast<unsigned char>(value), count);
    return dest;
}

void* __cdecl memmove(void* dest, const void* src, size_t count)
{
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    if (d <= s || d >= s + count) {
        __movsb(d, s, count);
    }
    else {
        while (count > 0) {
            --count;
            d[count] = s[count];
        }
    }
    return dest;
}

int __cdecl memcmp(const void* lhs, const void* rhs, size_t count)
{
    const auto* a = static_cast<const unsigned char*>(lhs);
    const auto* b = static_cast<const unsigned char*>(rhs);
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

}
