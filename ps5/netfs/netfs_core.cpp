// Snes9x PS5 netfs: the shares, their connections, the mount jobs and the watchdog (netfs.h, netfs_core.h).
//
// SPDX-License-Identifier: MIT
#include "netfs_core.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>

#include "ProsperoThread.h"

using namespace netfs;

namespace
{
std::vector<std::string> Segments(const std::string& path)
{
	std::vector<std::string> out;
	size_t i = 0;
	while (i <= path.size())
	{
		size_t e = path.find('/', i);
		if (e == std::string::npos)
			e = path.size();
		if (e > i)
			out.push_back(path.substr(i, e - i));
		i = e + 1;
	}
	return out;
}

using ShareList = std::vector<std::shared_ptr<Share>>;

// Everything global lives in one leaked object so no static destructor runs while a worker still uses it.
struct Registry
{
	std::mutex mu; // guards everything below
	std::shared_ptr<const ShareList> shares = std::make_shared<const ShareList>();
	std::vector<std::string> notices;
	std::string last_error;
	NetfsBackendFactory factory[2];
	std::function<int64_t()> clock;
	std::atomic<bool> custom_clock{false};
	std::atomic<bool> realtime{false};
	std::vector<ps5::BigThread> mounters;
	ps5::BigThread watchdog;
	std::condition_variable wd_cv;
	bool wd_stop = false;
};

Registry& R()
{
	static Registry* r = new Registry;
	return *r;
}

std::shared_ptr<const ShareList> Shares()
{
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	return r.shares;
}

std::unique_ptr<Backend> MakeBackend(Scheme s)
{
	NetfsBackendFactory f;
	{
		Registry& r = R();
		std::lock_guard<std::mutex> lk(r.mu);
		f = r.factory[(int)s];
	}
	if (f)
		return f();
	return s == Scheme::Smb ? MakeSmbBackend() : MakeNfsBackend();
}

void WatchdogLoop()
{
	Registry& r = R();
	std::unique_lock<std::mutex> lk(r.mu);
	while (!r.wd_stop)
	{
		lk.unlock();
		netfs_watchdog_tick();
		lk.lock();
		r.wd_cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return r.wd_stop; });
	}
}

bool ReadFile(const std::string& path, std::string& out, int& err)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
	{
		err = errno;
		return false;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

// Writes the file through <path>.part so a half-written template never stands in for it.
bool WriteNew(const std::string& path, const std::string& text)
{
	std::string part = path + ".part";
	FILE* f = fopen(part.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	ok = fclose(f) == 0 && ok;
	if (ok)
		ok = rename(part.c_str(), path.c_str()) == 0;
	if (!ok)
		remove(part.c_str());
	return ok;
}
} // namespace

namespace netfs
{
int64_t NowMs()
{
	Registry& r = R();
	if (r.custom_clock.load())
	{
		std::function<int64_t()> c;
		{
			std::lock_guard<std::mutex> lk(r.mu);
			c = r.clock;
		}
		if (c)
			return c();
	}
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() + 1; // never 0
}

void SetLastError(const std::string& text)
{
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	r.last_error = text;
}

// Marks a request in flight for the watchdog while it is alive.
class Timed
{
public:
	explicit Timed(Share& s) : m_s(s) { m_s.m_req_start.store(NowMs()); }
	~Timed() { m_s.m_req_start.store(0); }

private:
	Share& m_s;
};

Share::Share(const ShareConfig& c) : cfg(c), mount_point(netfs_mount_point(c)), label(netfs_redact(c)), segs(Segments(mount_point))
{
}

Share::~Share()
{
	if (m_backend)
		m_backend->Disconnect();
}

std::string Share::Error() const
{
	std::lock_guard<std::mutex> lk(m_state_mu);
	return m_error;
}

int Share::StateErrno() const
{
	switch (State())
	{
	case NetfsState::Ready:
		return 0;
	case NetfsState::Dead:
		return EIO;
	default:
		return ENOTCONN;
	}
}

void Share::Fail(NetfsState to, const Status& st)
{
	std::string text = netfs_error_text(st, cfg);
	{
		std::lock_guard<std::mutex> lk(m_state_mu);
		m_error = text;
		m_state.store(to);
	}
	SetLastError(text);
}

void Share::Mount()
{
	std::unique_ptr<Backend> b = MakeBackend(cfg.scheme);
	Backend* raw = b.get();
	{
		std::lock_guard<std::mutex> lk(m_state_mu);
		if (m_state.load() != NetfsState::Idle) // stopped before it began
			return;
		m_backend = std::move(b);
		m_state.store(NetfsState::Connecting);
	}
	Status st;
	{
		std::lock_guard<std::mutex> lk(m_conn_mu);
		Timed t(*this);
		st = raw->Connect(cfg);
		if (st.kind == ErrKind::ShareNotFound && st.available.empty() && m_state.load() == NetfsState::Connecting)
			raw->Enumerate(st.available);
	}
	std::lock_guard<std::mutex> lk(m_state_mu);
	if (m_state.load() != NetfsState::Connecting) // the watchdog or netfs_stop got there first
		return;
	if (st.ok())
		m_state.store(NetfsState::Ready);
	else
	{
		std::string text = netfs_error_text(st, cfg);
		m_error = text;
		m_state.store(NetfsState::Failed);
		SetLastError(text);
	}
}

void Share::Kill(bool stopping)
{
	Backend* b = nullptr;
	Status why = Status::Kind(ErrKind::Aborted);
	NetfsState to = NetfsState::Dead;
	{
		std::lock_guard<std::mutex> lk(m_state_mu);
		switch (m_state.load())
		{
		case NetfsState::Connecting:
			to = NetfsState::Failed;
			why = Status::Kind(ErrKind::NoAnswer);
			break;
		case NetfsState::Ready:
			to = NetfsState::Dead;
			break;
		case NetfsState::Idle:
			to = NetfsState::Failed; // never mounts now
			break;
		default:
			return;
		}
		b = m_backend.get();
		m_error = stopping ? std::string() : netfs_error_text(why, cfg);
		m_state.store(to);
	}
	if (!stopping)
		SetLastError(Error());
	if (b)
		b->Abort();
}

int Share::LockConn(std::unique_lock<std::mutex>& lk)
{
	if (int e = StateErrno())
		return e;
	lk = std::unique_lock<std::mutex>(m_conn_mu);
	return StateErrno(); // the watchdog may have cut the share while this call waited for the lock
}

Status Share::Stat(const std::string& rel, Attr& out)
{
	std::unique_lock<std::mutex> lk;
	if (int e = LockConn(lk))
		return Status::Errno(e);
	Timed t(*this);
	return m_backend->Stat(rel, out);
}

Status Share::List(const std::string& rel, std::vector<DirEntry>& out)
{
	std::unique_lock<std::mutex> lk;
	if (int e = LockConn(lk))
		return Status::Errno(e);
	Timed t(*this);
	return m_backend->List(rel, out);
}

Status Share::Open(const std::string& rel, Handle& out)
{
	std::unique_lock<std::mutex> lk;
	if (int e = LockConn(lk))
		return Status::Errno(e);
	Timed t(*this);
	return m_backend->Open(rel, out);
}

int64_t Share::Read(Handle h, uint64_t off, void* buf, size_t len)
{
	size_t done = 0;
	while (done < len)
	{
		std::unique_lock<std::mutex> lk;
		if (int e = LockConn(lk))
			return done ? (int64_t)done : -(int64_t)e;
		Timed t(*this);
		int64_t r = m_backend->Pread(h, (char*)buf + done, std::min<size_t>(len - done, 1u << 20), off + done);
		if (r < 0)
			return done ? (int64_t)done : r;
		if (r == 0)
			break;
		done += (size_t)r;
	}
	return (int64_t)done;
}

void Share::CloseHandle(Handle h)
{
	NetfsState s = State();
	if (s != NetfsState::Ready) // dead or failed: Disconnect() frees what is left
		return;
	std::lock_guard<std::mutex> lk(m_conn_mu);
	if (State() == NetfsState::Ready)
		m_backend->Close(h);
}

bool Resolve(const char* path, Route& out)
{
	if (!netfs_is_network_path(path))
		return false;
	std::vector<std::string> in;
	for (const std::string& s : Segments(path))
	{
		if (s == ".")
			continue;
		if (s == "..")
		{
			if (in.empty())
				return false;
			in.pop_back();
			continue;
		}
		in.push_back(s);
	}
	std::shared_ptr<const ShareList> list = Shares();
	const std::shared_ptr<Share>* best = nullptr;
	for (const std::shared_ptr<Share>& s : *list)
	{
		const std::vector<std::string>& m = s->segs;
		if (m.size() > in.size() || (best && m.size() <= (*best)->segs.size()))
			continue;
		if (std::equal(m.begin(), m.end(), in.begin()))
			best = &s;
	}
	if (!best)
		return false;
	out.share = *best;
	out.rel.clear();
	for (size_t i = (*best)->segs.size(); i < in.size(); i++)
		out.rel += (out.rel.empty() ? "" : "/") + in[i];
	return true;
}
} // namespace netfs

// ---- the public API (netfs.h) ----

int netfs_load_text(const std::string& text)
{
	netfs_stop();
	NetfsShareFile f = netfs_parse_shares(text);
	auto list = std::make_shared<ShareList>();
	for (const ShareConfig& c : f.shares)
		list->push_back(std::make_shared<Share>(c));
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	r.shares = list;
	r.notices = f.notices;
	r.last_error.clear();
	return (int)list->size();
}

int netfs_load(const std::string& shares_file)
{
	std::string text;
	int err = 0;
	if (ReadFile(shares_file, text, err))
		return netfs_load_text(text);
	netfs_stop();
	std::vector<std::string> notices;
	if (err == ENOENT)
	{
		if (!WriteNew(shares_file, netfs_shares_template()))
			notices.push_back("shares.txt could not be created");
	}
	else
		notices.push_back("shares.txt could not be read");
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	r.notices = notices;
	r.last_error.clear();
	return 0;
}

void netfs_start()
{
	std::shared_ptr<const ShareList> list = Shares();
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	r.wd_stop = false;
	for (const std::shared_ptr<Share>& s : *list)
	{
		std::shared_ptr<Share> keep = s;
		r.mounters.emplace_back([keep] { keep->Mount(); }, 512 * 1024);
	}
	if (!list->empty() && !r.watchdog.joinable())
		r.watchdog = ps5::BigThread(WatchdogLoop, 128 * 1024);
}

void netfs_mount_all_blocking()
{
	std::shared_ptr<const ShareList> list = Shares();
	for (const std::shared_ptr<Share>& s : *list)
		s->Mount();
}

void netfs_stop()
{
	Registry& r = R();
	std::shared_ptr<const ShareList> list;
	std::vector<ps5::BigThread> mounters;
	ps5::BigThread watchdog;
	{
		std::lock_guard<std::mutex> lk(r.mu);
		list = r.shares;
		r.shares = std::make_shared<const ShareList>();
		r.wd_stop = true;
		mounters.swap(r.mounters);
		watchdog = std::move(r.watchdog);
	}
	r.wd_cv.notify_all();
	for (const std::shared_ptr<Share>& s : *list)
		s->Kill(true);
	watchdog.join();
	for (ps5::BigThread& t : mounters)
		t.join();
}

std::vector<std::string> netfs_mount_points()
{
	std::vector<std::string> out;
	for (const std::shared_ptr<Share>& s : *Shares())
		if (s->State() == NetfsState::Ready)
			out.push_back(s->mount_point);
	return out;
}

std::vector<NetfsShareInfo> netfs_shares()
{
	std::vector<NetfsShareInfo> out;
	for (const std::shared_ptr<Share>& s : *Shares())
	{
		NetfsShareInfo i;
		i.mount_point = s->mount_point;
		i.label = s->label;
		i.state = s->State();
		i.error = s->Error();
		out.push_back(i);
	}
	return out;
}

bool netfs_mount_finished()
{
	for (const std::shared_ptr<Share>& s : *Shares())
	{
		NetfsState st = s->State();
		if (st == NetfsState::Idle || st == NetfsState::Connecting)
			return false;
	}
	return true;
}

std::string netfs_last_error()
{
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	return r.last_error;
}

std::vector<std::string> netfs_notices()
{
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	return r.notices;
}

bool netfs_is_network_path(const char* p)
{
	if (!p || p[0] != '/')
		return false;
	bool smb = p[1] == 's' && p[2] == 'm' && p[3] == 'b';
	bool nfs = p[1] == 'n' && p[2] == 'f' && p[3] == 's';
	return (smb || nfs) && p[4] == '/'; // the && chain stops at the string's end before p[4] is read
}

void netfs_set_realtime(bool on)
{
	R().realtime.store(on);
}

bool netfs_realtime()
{
	return R().realtime.load();
}

void netfs_set_backend_factory(Scheme scheme, NetfsBackendFactory factory)
{
	Registry& r = R();
	std::lock_guard<std::mutex> lk(r.mu);
	r.factory[(int)scheme] = std::move(factory);
}

void netfs_set_clock(std::function<int64_t()> now_ms)
{
	Registry& r = R();
	{
		std::lock_guard<std::mutex> lk(r.mu);
		r.clock = std::move(now_ms);
		r.custom_clock.store((bool)r.clock);
	}
}

void netfs_watchdog_tick()
{
	int64_t now = NowMs();
	for (const std::shared_ptr<Share>& s : *Shares())
	{
		int64_t t0 = s->RequestStart();
		if (!t0)
			continue;
		int64_t limit = s->State() == NetfsState::Connecting ? kNetfsConnectLimitMs : kNetfsRequestLimitMs;
		if (now - t0 > limit)
			s->Kill(false);
	}
}
