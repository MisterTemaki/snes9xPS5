// Snes9x PS5 netfs: test hooks of the NFS backend (backend_nfs.cpp). The backend itself is reached through
// netfs::MakeNfsBackend() in netfs_backend.h.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace netfs
{
// Lets a test run the backend's waits without sleeping (testing rules: no sleeping for timing). Set it before
// the first backend call; it is global and not thread safe.
struct NfsTestHooks
{
	int64_t (*now_ms)() = nullptr; // monotonic clock in ms; nullptr = the real one
	int poll_ms = 50;              // how long one wait step sleeps in poll(); tests use 0
};

void NfsSetTestHooks(const NfsTestHooks& hooks);
} // namespace netfs
