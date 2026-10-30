// Snes9x PS5: a std::thread-like worker with a chosen stack size.
//
// std::thread here maps to a pthread with the SDK's default stack, which is small. The cover worker runs
// the TLS handshake (libSceSsl) and a PNG/JPEG decode (stb_image), and the shelf's render threads run deep
// per-column math; 1.6 reached the shelf and then died with no log line, exactly what a worker-stack
// overflow looks like. BigThread gives each worker an explicit, generous stack.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "ProsperoCrash.h"

#include <cstdlib>
#include <functional>
#include <pthread.h>

namespace ps5
{
class BigThread
{
public:
	BigThread() = default;
	BigThread(const BigThread&) = delete;
	BigThread& operator=(const BigThread&) = delete;

	// Starts fn on a thread with a `stack_bytes` stack. Falls back to the default stack if that is refused.
	BigThread(std::function<void()> fn, size_t stack_bytes)
	{
#ifndef __PROSPERO__
		// the host tests: SNES9X_HOST_NO_THREADS=1 stands for a system that refuses every new thread
		if (const char* e = getenv("SNES9X_HOST_NO_THREADS"); e && *e == '1')
			return;
#endif
		auto* holder = new std::function<void()>(std::move(fn));
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, stack_bytes);
		if (pthread_create(&m_thread, &attr, &Trampoline, holder) != 0)
		{
			pthread_attr_destroy(&attr);
			if (pthread_create(&m_thread, nullptr, &Trampoline, holder) != 0)
			{
				delete holder;
				return;
			}
		}
		else
			pthread_attr_destroy(&attr);
		m_joinable = true;
	}

	BigThread(BigThread&& o) noexcept : m_thread(o.m_thread), m_joinable(o.m_joinable) { o.m_joinable = false; }
	BigThread& operator=(BigThread&& o) noexcept
	{
		if (this != &o)
		{
			if (m_joinable)
				pthread_join(m_thread, nullptr);
			m_thread = o.m_thread;
			m_joinable = o.m_joinable;
			o.m_joinable = false;
		}
		return *this;
	}
	~BigThread()
	{
		if (m_joinable)
			pthread_join(m_thread, nullptr);
	}

	bool joinable() const { return m_joinable; }
	void join()
	{
		if (m_joinable)
		{
			pthread_join(m_thread, nullptr);
			m_joinable = false;
		}
	}

private:
	static void* Trampoline(void* arg)
	{
		auto* holder = static_cast<std::function<void()>*>(arg);
		void* alt = crashlog::ArmThread(); // a crash on this thread is reported too, even a stack overflow
		(*holder)();
		crashlog::DisarmThread(alt);
		delete holder;
		return nullptr;
	}
	pthread_t m_thread{};
	bool m_joinable = false;
};
} // namespace ps5
