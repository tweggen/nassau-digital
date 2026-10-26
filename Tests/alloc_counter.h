#pragma once

// alloc_counter.h - a global operator new counter (the AllocTests pattern of
// nassau-analogue, Tests/synth_tests.cpp). Every scalar and array form of
// operator new is replaced and counted, so a delta across a region says
// whether that region allocated (R3: no allocation in process()).
//
// Include it in exactly ONE translation unit of a test executable: it defines
// the replacement operators.

#include <atomic>
#include <cstdlib>
#include <new>

namespace ndtest {
inline std::atomic<long long>& newCount() {
    static std::atomic<long long> n{0};
    return n;
}
}  // namespace ndtest

#if defined(__GNUC__) && !defined(__clang__)
// GCC pairs the replaced operator new with std::free below and warns, though
// both sides are ours and match.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t sz) {
    ++ndtest::newCount();
    if (void* p = std::malloc(sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t sz) {
    ++ndtest::newCount();
    if (void* p = std::malloc(sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
