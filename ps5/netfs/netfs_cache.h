// Snes9x PS5 netfs: what stands between the libc calls and the backend. Internal; netfs.h is the public side.
//
// Per share, one Cache holds
//  - folder listings (names with attributes): stat, lstat, access and open of a child are answered from the
//    parent's listing, and a name missing from a fresh listing is ENOENT with no round trip;
//  - the files being read, as 512 KiB blocks (.pcm audio: 128 KiB). A file of 16 MiB or less is fetched whole once
//    two blocks have been read in order; a bigger one keeps a window around the reader. A worker thread fetches
//    the blocks that are queued: read-ahead and the MSU-1 prefetch.
//
// While a game runs (netfs_realtime) nothing here waits on the network for the emulation thread, with the one
// exception the plan allows: a miss inside an MSU-1 data file waits for its block. See network-shares-spec.md,
// "Reads" and "Directory and attribute cache".
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ProsperoThread.h"
#include "netfs_core.h"

namespace netfs
{
constexpr size_t kBlockSize = 512 * 1024;
constexpr size_t kPcmBlockSize = 128 * 1024;    // a track's head, and the loop point's block
constexpr uint64_t kWholeFileMax = 16ull << 20; // fetched whole once read in order
constexpr int kMaxPcmHeads = 64;                // tracks prefetched per game (64 * 128 KiB = 8 MiB)
constexpr uint64_t kDataBudget = 32ull << 20;   // MSU-1 data files fetched whole before the first frame
constexpr int64_t kListingTtlMs = 60 * 1000;

class Cache;

struct Block
{
	enum State
	{
		Absent,
		Loading,
		Ready,
		Failed,
	};
	State st = Absent;
	int err = 0;
	bool pinned = false; // stays while the game runs
	bool queued = false; // a worker request for it is waiting
	std::vector<uint8_t> data;
};

// One file of a share, shared by every handle that has it open.
class FileData : public std::enable_shared_from_this<FileData>
{
public:
	FileData(Share& share, Cache& cache, const std::string& rel, const Attr& a);
	~FileData();

	const std::string rel;
	const uint64_t size;
	const int64_t mtime;
	const bool pcm;           // <rom>-<n>.pcm: a read that misses in realtime is silence, never a wait
	const size_t block_size;  // kPcmBlockSize or kBlockSize

	// Reads len bytes at off (fewer only at the end of the file or after an error that came after some
	// bytes). Returns the count, 0 at the end, or -errno.
	int64_t ReadAt(uint64_t off, void* buf, size_t len);

	bool BlockReady(size_t b);
	size_t BlockCount() const { return m_blocks.size(); }
	// Keeps the block through the game and queues it for the worker if it is not here.
	void Pin(size_t b);
	// Waits (real time) until block b is Ready or Failed or timeout_ms passed; returns its state then.
	Block::State WaitBlock(size_t b, int timeout_ms);
	// Queues a block for the worker (no-op when it is here or on its way).
	void Queue(size_t b);
	// Worker side: fetches block b if nobody has it yet.
	void FetchIfAbsent(size_t b);
	// The loop point of an MSU-1 track, from its head: pins and queues the block that holds it.
	void PinLoopPoint();
	void Unpin();

private:
	int EnsureOpen(Handle& h);
	int Fetch(size_t b); // b is claimed (Loading) by the caller
	size_t BlockLen(size_t b) const;
	void NoteAccess(size_t b); // m_mu held
	void Evict(size_t around); // m_mu held
	void QueueLocked(size_t b); // m_mu held

	Share& m_share;
	Cache& m_cache;
	std::mutex m_mu;
	std::condition_variable m_cv;
	std::vector<Block> m_blocks;
	std::set<size_t> m_resident; // blocks that are Ready
	size_t m_last = SIZE_MAX;
	int m_seq = 0;
	bool m_queued_all = false;
	std::mutex m_h_mu;
	Handle m_h = nullptr;
	bool m_opened = false;
	int m_open_err = 0;
};

struct CacheStats
{
	std::atomic<uint64_t> blocks_fetched{0};
	std::atomic<uint64_t> bytes_fetched{0};
	std::atomic<uint64_t> underruns{0};   // .pcm reads answered with silence
	std::atomic<uint64_t> data_stalls{0}; // MSU-1 data reads that waited for a block
};

class Cache
{
public:
	explicit Cache(Share& share);
	~Cache();

	// `actual`, when given, gets the path with the names as the server spells them (matched ignoring case).
	Status Stat(const std::string& rel, Attr& out, std::string* actual = nullptr);
	Status List(const std::string& rel, std::vector<DirEntry>& out);
	// The file's FileData (a new one when it changed on the server). In realtime a .pcm opens only when its
	// head is already here; every answer comes from memory.
	Status Open(const std::string& rel, std::shared_ptr<FileData>& out);
	void Invalidate(); // rescan: forget the listings

	// Before a game starts: fetches the MSU-1 files that sit beside the ROM. See netfs_prepare_game.
	void PrepareGame(const std::string& rom_rel, int head_wait_ms);
	void EndGame(); // lets go of what the game kept

	void Enqueue(const std::shared_ptr<FileData>& f, size_t block);
	void CloseLater(Handle h);
	void Stop(); // ends the worker thread; closes queued handles first
	void WaitIdle(); // tests: returns when the worker has nothing queued and nothing running

	CacheStats stats;

private:
	struct Listing
	{
		std::map<std::string, Attr> by_name;
		int64_t at = 0;
	};
	struct Req
	{
		std::weak_ptr<FileData> file;
		size_t block = 0;
	};

	Status GetListing(const std::string& rel, std::shared_ptr<const Listing>& out);
	std::shared_ptr<FileData> GetFile(const std::string& rel, const Attr& a);
	void EnsureWorker();
	void WorkerLoop();

	Share& m_share;
	std::mutex m_mu; // listings, files, kept
	std::map<std::string, std::shared_ptr<const Listing>> m_listings;
	std::map<std::string, std::weak_ptr<FileData>> m_files;
	std::vector<std::shared_ptr<FileData>> m_kept;
	bool m_root_valid = false;
	Attr m_root;

	std::mutex m_qmu;
	std::condition_variable m_qcv;
	std::deque<Req> m_queue;
	std::vector<Handle> m_closing;
	std::condition_variable m_idle_cv;
	bool m_busy = false; // the worker took a request or handles and has not finished them
	bool m_stop = false;
	bool m_worker_started = false;
	ps5::BigThread m_worker;
};

bool EndsWithNoCase(const std::string& s, const char* suffix);
} // namespace netfs
