/*
 * ps5-native-app-boilerplate - Minimal target C++ allocation runtime.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bridges standard C++ allocation operators to the clean-room libc module
 * without introducing RTTI.
 *
 * Snes9x PS5 2.10 (as Stella PS5 1.0.2): a failed allocation throws
 * std::bad_alloc, as the C++ standard says, instead of __builtin_trap()
 * (SIGILL). The eboot carries the whole C++ runtime since 2.10 (libc++abi,
 * libunwind; ps5/Makefile), so the exception reaches the catch the code has for
 * it -- the state loading catches it, where a damaged state file can ask for
 * gigabytes -- and one that nothing catches still ends in std::terminate, which
 * the crash log reports. This file is built with -fexceptions (ps5/Makefile,
 * APP_CRT_EH_*).
 */

#include <cstddef>
#include <new>

extern "C"
{
    void *malloc(std::size_t size);
    void free(void *address);
    int posix_memalign(void **address, std::size_t alignment, std::size_t size);
}

namespace
{
[[nodiscard]] void *allocate(std::size_t size) noexcept
{
    return malloc(size == 0 ? 1 : size);
}

[[nodiscard]] void *allocate_aligned(std::size_t size, std::size_t alignment) noexcept
{
    void *address = nullptr;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if ((alignment & (alignment - 1)) != 0)
        return nullptr;
    return posix_memalign(&address, alignment, size == 0 ? 1 : size) == 0 ? address : nullptr;
}

[[noreturn]] void allocation_failure()
{
    throw std::bad_alloc();
}
} // namespace

extern "C" __attribute__((noinline, visibility("hidden"))) bool
ps5ObserveOwnedAllocation(const void *address) noexcept
{
    __asm__ volatile("" : : "r"(address) : "memory");
    return address != nullptr;
}

void *operator new(std::size_t size)
{
    if (void *address = allocate(size))
        return address;
    allocation_failure();
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
    if (void *address = allocate_aligned(size, static_cast<std::size_t>(alignment)))
        return address;
    allocation_failure();
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *address) noexcept
{
    free(address);
}

void operator delete[](void *address) noexcept
{
    free(address);
}

void operator delete(void *address, std::size_t) noexcept
{
    free(address);
}

void operator delete[](void *address, std::size_t) noexcept
{
    free(address);
}

void operator delete(void *address, std::align_val_t) noexcept
{
    free(address);
}

void operator delete[](void *address, std::align_val_t) noexcept
{
    free(address);
}

void operator delete(void *address, std::size_t, std::align_val_t) noexcept
{
    free(address);
}

void operator delete[](void *address, std::size_t, std::align_val_t) noexcept
{
    free(address);
}

void operator delete(void *address, const std::nothrow_t &) noexcept
{
    free(address);
}

void operator delete[](void *address, const std::nothrow_t &) noexcept
{
    free(address);
}

void operator delete(void *address, std::align_val_t, const std::nothrow_t &) noexcept
{
    free(address);
}

void operator delete[](void *address, std::align_val_t, const std::nothrow_t &) noexcept
{
    free(address);
}
