// Snes9x PS5 netfs: listings, blocks, read-ahead and the MSU-1 prefetch (netfs_cache.h).
//
// SPDX-License-Identifier: MIT
#include "netfs_cache.h"

#include <strings.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>

namespace netfs
{
bool EndsWithNoCase(const std::string& s, const char* suffix)
{
	size_t n = strlen(suffix);
	return s.size() >= n && strncasecmp(s.c_str() + s.size() - n, suffix, n) == 0;
}

namespace
{
void SplitPath(const std::string& rel, std::string& parent, std::string& name)
{
	size_t slash = rel.rfind('/');
	parent = slash == std::string::npos ? std::string() : rel.substr(0, slash);
	name = slash == std::string::npos ? rel : rel.substr(slash + 1);
}

int ErrnoOf(const Status& s)
{
	return s.err ? s.err : EIO;
}

uint32_t Le32(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
} // namespace

// ---------------------------------------------------------------- FileData

FileData::FileData(Share& share, Cache& cache, const std::string& r, const Attr& a)
	: rel(r), size(a.size), mtime(a.mtime), pcm(EndsWithNoCase(r, ".pcm")), block_size(EndsWithNoCase(r, ".pcm") ? kPcmBlockSize : kBlockSize),
	  m_share(share), m_cache(cache)
{
	m_blocks.resize((size_t)((size + block_size - 1) / block_size));
}

FileData::~FileData()
{
	if (!m_opened || !m_h)
		return;
	if (netfs_realtime())
		m_cache.CloseLater(m_h); // the emulation thread does not wait for the connection's lock
	else
		m_share.CloseHandle(m_h);
}

size_t FileData::BlockLen(size_t b) const
{
	return (size_t)std::min<uint64_t>(block_size, size - (uint64_t)b * block_size);
}

int FileData::EnsureOpen(Handle& h)
{
	std::lock_guard<std::mutex> lk(m_h_mu);
	if (!m_opened)
	{
		m_opened = true;
		Status s = m_share.Open(rel, m_h);
		m_open_err = s.ok() ? 0 : ErrnoOf(s);
		if (!s.ok())
			m_h = nullptr;
	}
	h = m_h;
	return m_open_err;
}

int FileData::Fetch(size_t b)
{
	Handle h = nullptr;
	int err = EnsureOpen(h);
	std::vector<uint8_t> data;
	size_t len = BlockLen(b);
	if (!err)
	{
		data.resize(len);
		int64_t r = m_share.Read(h, (uint64_t)b * block_size, data.data(), len);
		if (r < 0)
			err = (int)-r;
		else if ((size_t)r < len)
			err = EIO; // the file is shorter than its listing said
	}
	{
		std::lock_guard<std::mutex> lk(m_mu);
		Block& blk = m_blocks[b];
		if (err)
		{
			blk.st = Block::Failed;
			blk.err = err;
		}
		else
		{
			blk.data = std::move(data);
			blk.st = Block::Ready;
			m_resident.insert(b);
			m_cache.stats.blocks_fetched++;
			m_cache.stats.bytes_fetched += len;
		}
	}
	m_cv.notify_all();
	return err;
}

void FileData::QueueLocked(size_t b)
{
	Block& blk = m_blocks[b];
	if (blk.st == Block::Absent && !blk.queued)
	{
		blk.queued = true;
		m_cache.Enqueue(shared_from_this(), b);
	}
}

void FileData::Queue(size_t b)
{
	std::lock_guard<std::mutex> lk(m_mu);
	QueueLocked(b);
}

void FileData::FetchIfAbsent(size_t b)
{
	{
		std::lock_guard<std::mutex> lk(m_mu);
		Block& blk = m_blocks[b];
		blk.queued = false;
		if (blk.st != Block::Absent)
			return;
		blk.st = Block::Loading;
	}
	Fetch(b);
}

void FileData::Evict(size_t around)
{
	const size_t ahead = pcm ? 32 : 8;
	for (auto it = m_resident.begin(); it != m_resident.end();)
	{
		size_t c = *it;
		Block& k = m_blocks[c];
		if (!k.pinned && (c + 2 < around || c > around + ahead + 2))
		{
			std::vector<uint8_t>().swap(k.data);
			k.st = Block::Absent;
			k.queued = false;
			it = m_resident.erase(it);
		}
		else
			++it;
	}
}

// Called with the lock held after a block was read from. Two blocks in a row start the read-ahead.
void FileData::NoteAccess(size_t b)
{
	if (b == m_last)
		return;
	m_seq = (m_last != SIZE_MAX && b == m_last + 1) ? m_seq + 1 : 0;
	m_last = b;
	const bool whole = size <= kWholeFileMax;
	if (m_seq >= 1 || pcm) // a track is read in order: no need to see two blocks first
	{
		if (whole)
		{
			if (!m_queued_all)
			{
				m_queued_all = true;
				for (size_t c = b + 1; c < m_blocks.size(); c++)
					QueueLocked(c);
			}
		}
		else
		{
			const size_t ahead = pcm ? 32 : 8;
			for (size_t c = b + 1; c <= b + ahead && c < m_blocks.size(); c++)
				QueueLocked(c);
		}
	}
	if (!whole)
		Evict(b);
}

int64_t FileData::ReadAt(uint64_t off, void* dst, size_t len)
{
	if (off >= size || !len)
		return 0;
	len = (size_t)std::min<uint64_t>(len, size - off);
	const bool rt = netfs_realtime();
	uint8_t* out = (uint8_t*)dst;
	size_t done = 0;
	int64_t result = -1;
	bool stalled = false, failed = false;
	std::string note;
	std::unique_lock<std::mutex> lk(m_mu);
	while (done < len)
	{
		uint64_t pos = off + done;
		size_t b = (size_t)(pos / block_size);
		Block& blk = m_blocks[b];
		if (blk.st == Block::Ready)
		{
			size_t in = (size_t)(pos - (uint64_t)b * block_size);
			size_t n = std::min(len - done, blk.data.size() - in);
			memcpy(out + done, blk.data.data() + in, n);
			done += n;
			NoteAccess(b);
			continue;
		}
		if (pcm && rt)
		{
			// Silence for the whole rest of the span, never a short read (the core would end the track).
			memset(out + done, 0, len - done);
			done = len;
			QueueLocked(b);
			uint64_t n = ++m_cache.stats.underruns;
			if (n == 1 || n % 100 == 0)
				note = m_share.label + ": audio underrun #" + std::to_string(n) + " in " + rel;
			break;
		}
		if (rt && !stalled)
		{
			stalled = true;
			uint64_t n = ++m_cache.stats.data_stalls;
			if (n == 1 || n % 100 == 0)
				note = m_share.label + ": waited for " + rel + " in realtime (#" + std::to_string(n) + ")";
		}
		if (blk.st == Block::Loading)
		{
			m_cv.wait(lk, [&] { return m_blocks[b].st != Block::Loading; });
			continue;
		}
		blk.st = Block::Loading; // Absent, or Failed once: fetch it here
		blk.queued = false;
		lk.unlock();
		int err = Fetch(b);
		lk.lock();
		if (err)
		{
			failed = true;
			result = done ? (int64_t)done : -(int64_t)err;
			break;
		}
	}
	lk.unlock();
	if (!note.empty())
		Log(note);
	return failed ? result : (int64_t)done;
}

bool FileData::BlockReady(size_t b)
{
	std::lock_guard<std::mutex> lk(m_mu);
	return b < m_blocks.size() && m_blocks[b].st == Block::Ready;
}

void FileData::Pin(size_t b)
{
	std::lock_guard<std::mutex> lk(m_mu);
	if (b >= m_blocks.size())
		return;
	m_blocks[b].pinned = true;
	QueueLocked(b);
}

void FileData::Unpin()
{
	std::lock_guard<std::mutex> lk(m_mu);
	for (Block& k : m_blocks)
		k.pinned = false;
}

Block::State FileData::WaitBlock(size_t b, int timeout_ms)
{
	std::unique_lock<std::mutex> lk(m_mu);
	if (b >= m_blocks.size())
		return Block::Failed;
	m_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return m_blocks[b].st == Block::Ready || m_blocks[b].st == Block::Failed; });
	return m_blocks[b].st;
}

// An MSU-1 track starts "MSU1", then the loop point in samples: byte 8 + 4 * le32. The block that holds it is
// kept so a loop costs no wait.
void FileData::PinLoopPoint()
{
	std::lock_guard<std::mutex> lk(m_mu);
	if (m_blocks.empty() || m_blocks[0].st != Block::Ready || m_blocks[0].data.size() < 8 || memcmp(m_blocks[0].data.data(), "MSU1", 4) != 0)
		return;
	uint64_t at = 8 + 4ull * Le32(m_blocks[0].data.data() + 4);
	if (at >= size)
		return;
	size_t b = (size_t)(at / block_size);
	m_blocks[b].pinned = true;
	QueueLocked(b);
}

// ---------------------------------------------------------------- Cache

Cache::Cache(Share& share) : m_share(share) {}

Cache::~Cache()
{
	Stop();
	// Before the members go: a FileData closes its handle through the queue, which must still be there.
	std::vector<std::shared_ptr<FileData>> kept;
	{
		std::lock_guard<std::mutex> lk(m_mu);
		kept.swap(m_kept);
		m_files.clear();
	}
}

void Cache::Invalidate()
{
	std::lock_guard<std::mutex> lk(m_mu);
	m_listings.clear();
	m_root_valid = false;
}

Status Cache::GetListing(const std::string& rel, std::shared_ptr<const Listing>& out)
{
	const bool rt = netfs_realtime();
	{
		std::lock_guard<std::mutex> lk(m_mu);
		auto it = m_listings.find(rel);
		if (it != m_listings.end() && (rt || (m_share.StateErrno() == 0 && NowMs() - it->second->at <= kListingTtlMs)))
		{
			out = it->second;
			return Status::Ok();
		}
	}
	if (rt)
		return Status::Errno(ENOENT); // not in memory: unknown, and the emulation thread does not ask the network
	if (int e = m_share.StateErrno())
		return Status::Errno(e);
	std::vector<DirEntry> v;
	Status s = m_share.List(rel, v);
	if (!s.ok())
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_listings.erase(rel);
		return s;
	}
	auto l = std::make_shared<Listing>();
	l->at = NowMs();
	for (DirEntry& e : v)
		l->by_name[e.name] = e.attr;
	std::lock_guard<std::mutex> lk(m_mu);
	m_listings[rel] = l;
	out = l;
	return Status::Ok();
}

Status Cache::Stat(const std::string& rel, Attr& out, std::string* actual)
{
	if (actual)
		*actual = rel;
	if (rel.empty())
	{
		{
			std::lock_guard<std::mutex> lk(m_mu);
			if (m_root_valid)
			{
				out = m_root;
				return Status::Ok();
			}
		}
		if (netfs_realtime())
			return Status::Errno(ENOENT);
		Status s = m_share.Stat("", out);
		if (s.ok())
		{
			std::lock_guard<std::mutex> lk(m_mu);
			m_root = out;
			m_root_valid = true;
		}
		return s;
	}
	std::string parent, name;
	SplitPath(rel, parent, name);
	std::shared_ptr<const Listing> l;
	Status s = GetListing(parent, l);
	if (!s.ok())
		return s;
	auto it = l->by_name.find(name);
	if (it == l->by_name.end())
	{
		// SMB shares ignore case; so do we, as long as one name fits
		auto fit = l->by_name.end();
		for (auto e = l->by_name.begin(); e != l->by_name.end(); ++e)
			if (strcasecmp(e->first.c_str(), name.c_str()) == 0)
			{
				if (fit != l->by_name.end())
					return Status::Errno(ENOENT); // two names differ only in case: not ours to pick
				fit = e;
			}
		if (fit == l->by_name.end())
			return Status::Errno(ENOENT); // a name missing from a fresh listing: no round trip
		it = fit;
		if (actual)
			*actual = parent.empty() ? it->first : parent + "/" + it->first;
	}
	out = it->second;
	return Status::Ok();
}

Status Cache::List(const std::string& rel, std::vector<DirEntry>& out)
{
	std::shared_ptr<const Listing> l;
	Status s = GetListing(rel, l);
	if (!s.ok())
		return s;
	out.clear();
	for (const auto& e : l->by_name)
	{
		DirEntry d;
		d.name = e.first;
		d.attr = e.second;
		out.push_back(d);
	}
	return Status::Ok();
}

std::shared_ptr<FileData> Cache::GetFile(const std::string& rel, const Attr& a)
{
	std::lock_guard<std::mutex> lk(m_mu);
	auto it = m_files.find(rel);
	if (it != m_files.end())
	{
		std::shared_ptr<FileData> f = it->second.lock();
		if (f && f->size == a.size && f->mtime == a.mtime)
			return f;
	}
	if (m_files.size() > 256)
		for (auto e = m_files.begin(); e != m_files.end();)
			e = e->second.expired() ? m_files.erase(e) : std::next(e);
	auto f = std::make_shared<FileData>(m_share, *this, rel, a);
	m_files[rel] = f;
	return f;
}

Status Cache::Open(const std::string& rel, std::shared_ptr<FileData>& out)
{
	Attr a;
	std::string path;
	Status s = Stat(rel, a, &path);
	if (!s.ok())
		return s;
	if (a.is_dir)
		return Status::Errno(EISDIR);
	std::shared_ptr<FileData> f = GetFile(path, a);
	EnsureWorker();
	if (f->pcm)
	{
		if (netfs_realtime() && !f->BlockReady(0))
		{
			f->Pin(0); // for the next time the game asks
			std::lock_guard<std::mutex> lk(m_mu);
			if (std::find(m_kept.begin(), m_kept.end(), f) == m_kept.end())
				m_kept.push_back(f); // alive until the game ends, so the fetch can finish
			Log(m_share.label + ": track not cached yet, open refused: " + path);
			return Status::Errno(ENOENT);
		}
		f->PinLoopPoint();
	}
	out = f;
	return Status::Ok();
}

void Cache::PrepareGame(const std::string& rom_rel, int head_wait_ms)
{
	std::string folder, name;
	SplitPath(rom_rel, folder, name);
	size_t dot = name.rfind('.');
	const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
	std::shared_ptr<const Listing> l;
	if (!GetListing(folder, l).ok())
		return;
	EnsureWorker();
	std::vector<std::shared_ptr<FileData>> heads, data;
	uint64_t data_bytes = 0;
	for (const auto& e : l->by_name)
	{
		const std::string& n = e.first;
		const Attr& a = e.second;
		if (a.is_dir || a.size == 0)
			continue;
		const std::string path = folder.empty() ? n : folder + "/" + n;
		bool is_pcm = n.size() > stem.size() + 1 && n.compare(0, stem.size() + 1, stem + "-") == 0 && EndsWithNoCase(n, ".pcm");
		bool is_data = n == stem + ".msu" || n == "msu1.rom";
		if (is_pcm && (int)heads.size() < kMaxPcmHeads)
		{
			std::shared_ptr<FileData> f = GetFile(path, a);
			f->Pin(0);
			heads.push_back(f);
		}
		else if (is_data && data_bytes + a.size <= kDataBudget)
		{
			data_bytes += a.size;
			std::shared_ptr<FileData> f = GetFile(path, a);
			for (size_t b = 0; b < f->BlockCount(); b++)
				f->Pin(b);
			data.push_back(f);
		}
	}
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_kept.insert(m_kept.end(), heads.begin(), heads.end());
		m_kept.insert(m_kept.end(), data.begin(), data.end());
	}
	// Game data is never skipped: wait for all of it. A dead share fails the blocks or stops being Ready, so this ends.
	for (const std::shared_ptr<FileData>& f : data)
		for (size_t b = 0; b < f->BlockCount(); b++)
			for (;;)
			{
				Block::State st = f->WaitBlock(b, 250);
				if (st == Block::Ready || st == Block::Failed || m_share.StateErrno())
					break;
			}
	// Track heads: as many as arrive within the deadline.
	using namespace std::chrono;
	const auto until = steady_clock::now() + milliseconds(head_wait_ms);
	size_t ready = 0;
	for (const std::shared_ptr<FileData>& f : heads)
	{
		int64_t left = duration_cast<milliseconds>(until - steady_clock::now()).count();
		if (f->WaitBlock(0, (int)std::max<int64_t>(left, 0)) == Block::Ready)
		{
			ready++;
			f->PinLoopPoint();
		}
	}
	if (!heads.empty() || data_bytes)
		Log(m_share.label + ": prefetch for " + name + ": " + std::to_string(ready) + "/" + std::to_string(heads.size()) + " track heads, " +
			std::to_string(data_bytes) + " bytes of MSU-1 data");
}

void Cache::EndGame()
{
	std::vector<std::shared_ptr<FileData>> kept;
	{
		std::lock_guard<std::mutex> lk(m_mu);
		kept.swap(m_kept);
	}
	for (const std::shared_ptr<FileData>& f : kept)
		f->Unpin();
}

void Cache::EnsureWorker()
{
	std::lock_guard<std::mutex> lk(m_qmu);
	if (m_worker_started || m_stop)
		return;
	m_worker_started = true;
	m_worker = ps5::BigThread([this] { WorkerLoop(); }, 256 * 1024);
}

void Cache::Enqueue(const std::shared_ptr<FileData>& f, size_t block)
{
	{
		std::lock_guard<std::mutex> lk(m_qmu);
		if (m_stop)
			return;
		Req r;
		r.file = f;
		r.block = block;
		m_queue.push_back(r);
	}
	EnsureWorker();
	m_qcv.notify_one();
}

void Cache::CloseLater(Handle h)
{
	{
		std::lock_guard<std::mutex> lk(m_qmu);
		if (!m_stop)
		{
			m_closing.push_back(h);
			h = nullptr;
		}
	}
	if (h)
	{
		m_share.CloseHandle(h); // stopped: nobody else will close it
		return;
	}
	EnsureWorker();
	m_qcv.notify_one();
}

void Cache::WorkerLoop()
{
	for (;;)
	{
		Req r;
		bool have = false, stop = false;
		std::vector<Handle> closing;
		{
			std::unique_lock<std::mutex> lk(m_qmu);
			m_qcv.wait(lk, [&] { return m_stop || !m_queue.empty() || !m_closing.empty(); });
			closing.swap(m_closing);
			stop = m_stop;
			if (!stop && !m_queue.empty())
			{
				r = m_queue.front();
				m_queue.pop_front();
				have = true;
			}
			m_busy = have || !closing.empty();
		}
		for (Handle h : closing)
			m_share.CloseHandle(h);
		if (stop)
			return;
		if (have)
			if (std::shared_ptr<FileData> f = r.file.lock())
				f->FetchIfAbsent(r.block);
		{
			std::lock_guard<std::mutex> lk(m_qmu);
			m_busy = false;
		}
		m_idle_cv.notify_all();
	}
}

void Cache::WaitIdle()
{
	std::unique_lock<std::mutex> lk(m_qmu);
	m_idle_cv.wait(lk, [&] { return m_stop || (m_queue.empty() && m_closing.empty() && !m_busy); });
}

void Cache::Stop()
{
	{
		std::lock_guard<std::mutex> lk(m_qmu);
		m_stop = true;
		m_queue.clear();
	}
	m_qcv.notify_all();
	m_worker.join();
	std::vector<Handle> closing;
	{
		std::lock_guard<std::mutex> lk(m_qmu);
		closing.swap(m_closing);
	}
	for (Handle h : closing)
		m_share.CloseHandle(h);
}
} // namespace netfs
