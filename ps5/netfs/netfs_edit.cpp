// Snes9x PS5 netfs: what the Network screen needs besides the shares themselves -- reading and editing
// shares.txt line by line, and a one-off connection test (netfs.h).
//
// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstring>
#include <mutex>

#include "ProsperoThread.h"
#include "netfs_core.h"

using namespace netfs;

namespace
{
const char* const kChanged = "shares.txt changed while this was open. Go back and open the share again.";

// Reads the file for a change. A file that does not exist yet starts from the template.
std::string LoadForEdit(std::string& text)
{
	const std::string file = SharesFilePath();
	if (file.empty())
		return "The app has no /data/snes9x folder, so shares cannot be kept.";
	int err = 0;
	if (ReadWholeFile(file, text, err))
		return std::string();
	if (err != ENOENT)
		return "shares.txt could not be read.";
	text = netfs_shares_template();
	return std::string();
}

std::string Store(const std::string& text)
{
	return WriteFileAtomic(SharesFilePath(), text) ? std::string() : "shares.txt could not be saved.";
}

// The line the screen showed (same line number, still a valid address, still the same folder), or nullptr.
const NetfsShareLine* Find(const std::vector<NetfsShareLine>& lines, int line, const std::string& mount_point)
{
	for (const NetfsShareLine& l : lines)
		if (l.line == line)
			return l.ok && netfs_mount_point(l.cfg) == mount_point ? &l : nullptr;
	return nullptr;
}

std::string Count(size_t n, const char* noun)
{
	return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}
} // namespace

NetfsFileView netfs_file_shares()
{
	NetfsFileView v;
	std::string text;
	v.error = LoadForEdit(text);
	if (!v.error.empty())
		return v;
	for (const NetfsShareLine& l : netfs_scan_shares(text))
	{
		if (!l.ok)
		{
			v.invalid++;
			continue;
		}
		NetfsFileShare s;
		s.line = l.line;
		s.cfg = l.cfg;
		s.mount_point = netfs_mount_point(l.cfg);
		s.used = l.used;
		s.note = l.ignored;
		v.shares.push_back(std::move(s));
	}
	return v;
}

std::string netfs_file_save_share(int line, const std::string& old_mount_point, const ShareConfig& cfg)
{
	std::string text;
	std::string err = LoadForEdit(text);
	if (!err.empty())
		return err;
	std::vector<NetfsShareLine> lines = netfs_scan_shares(text);
	const NetfsShareLine* old = nullptr;
	if (line > 0)
	{
		old = Find(lines, line, old_mount_point);
		if (!old)
			return kChanged;
	}
	const std::string mp = netfs_mount_point(cfg);
	int used = 0;
	for (const NetfsShareLine& l : lines)
	{
		if (!l.ok || !l.used)
			continue;
		used++;
		if (l.line != line && netfs_mount_point(l.cfg) == mp)
			return "Another share already uses that folder.";
	}
	if (!(old && old->used) && used >= kNetfsMaxShares)
		return "Up to " + std::to_string(kNetfsMaxShares) + " shares are used. Remove one first.";
	if (!netfs_shares_text_set(text, line, netfs_format_share(cfg)))
		return kChanged;
	err = Store(text);
	if (err.empty())
		Log("shares.txt: saved " + netfs_redact(cfg));
	return err;
}

std::string netfs_file_remove_share(int line, const std::string& old_mount_point)
{
	std::string text;
	std::string err = LoadForEdit(text);
	if (!err.empty())
		return err;
	const std::vector<NetfsShareLine> lines = netfs_scan_shares(text);
	const NetfsShareLine* old = Find(lines, line, old_mount_point);
	if (!old || !netfs_shares_text_remove(text, line))
		return kChanged;
	const std::string label = netfs_redact(old->cfg);
	err = Store(text);
	if (err.empty())
		Log("shares.txt: removed " + label);
	return err;
}

// ---- the connection test ----

struct NetfsConnectionTest::Impl
{
	ShareConfig cfg;
	std::unique_ptr<Backend> backend;
	int64_t deadline = 0;

	std::mutex mu;         // guards the three flags below and every Abort()
	bool finished = false; // the worker is past its last call an Abort() could meet; none is made after this
	bool aborted = false;
	bool timed_out = false;

	std::atomic<bool> done{false}; // result is complete
	NetfsTestResult result;
	ps5::BigThread thread; // last: joined first

	void Run()
	{
		Status st = backend->Connect(cfg);
		if (st.kind == ErrKind::ShareNotFound && st.available.empty())
			backend->Enumerate(st.available);
		size_t files = 0, dirs = 0;
		Status ls;
		if (st.ok())
		{
			std::vector<DirEntry> entries;
			ls = backend->List("", entries);
			for (const DirEntry& e : entries)
				(e.attr.is_dir ? dirs : files)++;
		}
		bool timed;
		{
			std::lock_guard<std::mutex> lk(mu);
			finished = true;
			timed = timed_out;
		}
		NetfsTestResult r;
		if (timed)
			st = Status::Kind(ErrKind::NoAnswer);
		if (!st.ok())
			r.text = netfs_error_text(st, cfg);
		else if (!ls.ok())
			r.text = std::string("Connected, but the folder could not be read (") + (ls.err ? strerror(ls.err) : "error") + ").";
		else if (files + dirs == 0)
			r.text = "Connected, but the folder is empty.";
		else
		{
			r.ok = true;
			r.text = "Connected. The folder has " + Count(files, "file") + " and " + Count(dirs, "folder") + ".";
		}
		backend->Disconnect();
		result = std::move(r);
		done.store(true);
	}

	void AbortNow(bool timeout)
	{
		std::lock_guard<std::mutex> lk(mu);
		if (finished || aborted)
			return;
		aborted = true;
		timed_out = timeout;
		backend->Abort();
	}
};

NetfsConnectionTest::NetfsConnectionTest(const ShareConfig& cfg) : m(new Impl)
{
	m->cfg = cfg;
	m->backend = MakeBackendFor(cfg.scheme);
	m->deadline = NowMs() + kNetfsConnectLimitMs;
	Impl* impl = m.get();
	m->thread = ps5::BigThread([impl] { impl->Run(); }, 512 * 1024);
}

NetfsConnectionTest::~NetfsConnectionTest()
{
	if (m && !m->done.load())
		m->AbortNow(false);
}

bool NetfsConnectionTest::Poll()
{
	if (m->done.load())
		return true;
	if (NowMs() > m->deadline)
		m->AbortNow(true);
	return false;
}

void NetfsConnectionTest::Cancel()
{
	if (!m->done.load())
		m->AbortNow(false);
}

const NetfsTestResult& NetfsConnectionTest::Result() const
{
	return m->result;
}
