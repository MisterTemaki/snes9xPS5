// Snes9x PS5 netfs tests: an in-memory share that speaks the netfs::Backend interface.
//
// A FakeServer is the share's content and its switches; any number of FakeBackends (one per mount) read it.
// Counters say how much traffic a test caused; `hang` and `connect_status` make the server misbehave. Nothing
// here sleeps: a hung call waits on a condition until Abort() (the watchdog's kill) releases it.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "netfs_backend.h"

struct FakeServer
{
	std::mutex mu;
	std::condition_variable cv;
	std::map<std::string, std::string> files; // path relative to the location -> content
	std::set<std::string> dirs;               // "" is the location itself
	int64_t mtime = 1700000000;

	// switches
	netfs::Status connect_status;      // what Connect() returns
	std::atomic<bool> hang{false};     // the next call waits until its backend is aborted
	std::atomic<bool> fail_reads{false}; // Pread() returns -EIO
	std::atomic<bool> gate{false};     // Pread() waits until Release() (a slow link) or its backend is aborted
	std::atomic<int> gated{0};         // reads waiting at the gate
	std::vector<std::string> offered;  // Enumerate()

	// traffic
	std::atomic<int> connects{0}, stats{0}, lists{0}, opens{0}, reads{0}, closes{0};
	std::atomic<uint64_t> bytes_read{0};
	std::atomic<int> waiting{0}; // calls parked by `hang`

	FakeServer() { dirs.insert(""); }

	void Put(const std::string& path, const std::string& data)
	{
		std::lock_guard<std::mutex> lk(mu);
		files[path] = data;
		for (size_t i = path.find('/'); i != std::string::npos; i = path.find('/', i + 1))
			dirs.insert(path.substr(0, i));
	}

	void Release()
	{
		{
			std::lock_guard<std::mutex> lk(mu);
			gate = false;
		}
		cv.notify_all();
	}

	void WaitGated(int n)
	{
		std::unique_lock<std::mutex> lk(mu);
		cv.wait(lk, [&] { return gated.load() >= n; });
	}

	// Waits (a real condition wait, no sleep) until `n` calls are parked by `hang`.
	void WaitParked(int n)
	{
		std::unique_lock<std::mutex> lk(mu);
		cv.wait(lk, [&] { return waiting.load() >= n; });
	}
};

class FakeBackend : public netfs::Backend
{
public:
	explicit FakeBackend(std::shared_ptr<FakeServer> s) : m_s(std::move(s)) {}
	~FakeBackend() override { Disconnect(); }

	netfs::Status Connect(const netfs::ShareConfig& cfg) override
	{
		m_s->connects++;
		m_cfg = cfg;
		if (Parked())
			return netfs::Status::Kind(netfs::ErrKind::Aborted);
		return m_s->connect_status;
	}

	void Disconnect() override
	{
		for (OpenRec* o : m_open)
			delete o;
		m_open.clear();
	}

	netfs::Status Stat(const std::string& path, netfs::Attr& out) override
	{
		m_s->stats++;
		if (Parked())
			return netfs::Status::Errno(EIO);
		std::lock_guard<std::mutex> lk(m_s->mu);
		return StatLocked(path, out);
	}

	netfs::Status List(const std::string& path, std::vector<netfs::DirEntry>& out) override
	{
		m_s->lists++;
		if (Parked())
			return netfs::Status::Errno(EIO);
		std::lock_guard<std::mutex> lk(m_s->mu);
		netfs::Attr a;
		netfs::Status st = StatLocked(path, a);
		if (!st.ok())
			return st;
		if (!a.is_dir)
			return netfs::Status::Errno(ENOTDIR);
		std::string prefix = path.empty() ? "" : path + "/";
		auto direct_child = [&](const std::string& p, std::string& name) {
			if (p.size() <= prefix.size() || p.compare(0, prefix.size(), prefix) != 0)
				return false;
			name = p.substr(prefix.size());
			return name.find('/') == std::string::npos;
		};
		for (const auto& d : m_s->dirs)
		{
			netfs::DirEntry e;
			if (direct_child(d, e.name))
			{
				e.attr.is_dir = true;
				e.attr.mtime = m_s->mtime;
				out.push_back(e);
			}
		}
		for (const auto& f : m_s->files)
		{
			netfs::DirEntry e;
			if (direct_child(f.first, e.name))
			{
				e.attr.size = f.second.size();
				e.attr.mtime = m_s->mtime;
				out.push_back(e);
			}
		}
		return netfs::Status::Ok();
	}

	netfs::Status Open(const std::string& path, netfs::Handle& out) override
	{
		m_s->opens++;
		if (Parked())
			return netfs::Status::Errno(EIO);
		std::lock_guard<std::mutex> lk(m_s->mu);
		auto it = m_s->files.find(path);
		if (it == m_s->files.end())
			return netfs::Status::Errno(m_s->dirs.count(path) ? EISDIR : ENOENT);
		OpenRec* o = new OpenRec{path};
		m_open.insert(o);
		out = o;
		return netfs::Status::Ok();
	}

	int64_t Pread(netfs::Handle h, void* buf, size_t len, uint64_t offset) override
	{
		m_s->reads++;
		if (Parked() || AtGate() || m_s->fail_reads)
			return -EIO;
		std::lock_guard<std::mutex> lk(m_s->mu);
		const std::string& data = m_s->files[((OpenRec*)h)->path];
		if (offset >= data.size())
			return 0;
		size_t n = std::min<size_t>(len, data.size() - offset);
		n = std::min<size_t>(n, 300 * 1024); // short reads: netfs must ask again
		memcpy(buf, data.data() + offset, n);
		m_s->bytes_read += n;
		return (int64_t)n;
	}

	void Close(netfs::Handle h) override
	{
		m_s->closes++;
		m_open.erase((OpenRec*)h);
		delete (OpenRec*)h;
	}

	void Abort() override
	{
		{
			std::lock_guard<std::mutex> lk(m_s->mu);
			m_aborted = true;
		}
		m_s->cv.notify_all();
	}

	netfs::Status Enumerate(std::vector<std::string>& out) override
	{
		out = m_s->offered;
		return netfs::Status::Ok();
	}

	netfs::ShareConfig m_cfg;

private:
	struct OpenRec
	{
		std::string path;
	};

	// With `gate` set: waits until Release() or Abort(). Returns true when the call was aborted.
	bool AtGate()
	{
		std::unique_lock<std::mutex> lk(m_s->mu);
		if (m_s->gate && !m_aborted)
		{
			m_s->gated++;
			m_s->cv.notify_all();
			m_s->cv.wait(lk, [&] { return !m_s->gate || m_aborted; });
			m_s->gated--;
		}
		return m_aborted;
	}

	// With `hang` set: parks the call until Abort(). Returns true when the call was aborted.
	bool Parked()
	{
		std::unique_lock<std::mutex> lk(m_s->mu);
		if (m_s->hang && !m_aborted)
		{
			m_s->waiting++;
			m_s->cv.notify_all();
			m_s->cv.wait(lk, [&] { return m_aborted; });
			m_s->waiting--;
		}
		return m_aborted;
	}

	netfs::Status StatLocked(const std::string& path, netfs::Attr& out)
	{
		auto f = m_s->files.find(path);
		if (f != m_s->files.end())
		{
			out.is_dir = false;
			out.size = f->second.size();
		}
		else if (m_s->dirs.count(path))
			out.is_dir = true;
		else
			return netfs::Status::Errno(ENOENT);
		out.mtime = m_s->mtime;
		return netfs::Status::Ok();
	}

	std::shared_ptr<FakeServer> m_s;
	std::set<OpenRec*> m_open;
	bool m_aborted = false; // guarded by m_s->mu
};
