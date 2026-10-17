// Snes9x PS5 netfs: the libc file calls for network paths (netfs_vfs.h).
//
// SPDX-License-Identifier: MIT
#include "netfs_vfs.h"

#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include "netfs_cache.h"
#include "netfs_core.h"

namespace netfs
{
namespace vfs
{
namespace
{
struct OpenFile
{
	std::recursive_mutex mu; // the stdio per-FILE lock too (flockfile)
	bool used = false;       // guarded by g_slots_mu
	std::shared_ptr<Share> share; // outlives data: the cache's handle closes through it
	std::shared_ptr<FileData> data;
	uint64_t size = 0;
	uint64_t pos = 0; // the next byte a read returns
	bool eof = false;
	bool err = false;
	int pushback = -1;
};

struct OpenDir
{
	bool used = false; // guarded by g_slots_mu
	std::vector<DirEntry> entries;
	size_t next = 0;
	struct dirent cur;
};

// Static storage, constructed on first use and never destroyed: a FILE* is only ever netfs's after an open
// has run, so the range tests below need no construction, and no destructor runs at exit.
alignas(OpenFile) unsigned char g_file_mem[kMaxFiles * sizeof(OpenFile)];
alignas(OpenDir) unsigned char g_dir_mem[kMaxDirs * sizeof(OpenDir)];
std::mutex g_slots_mu;
std::once_flag g_once;

OpenFile* Files() { return reinterpret_cast<OpenFile*>(g_file_mem); }
OpenDir* Dirs() { return reinterpret_cast<OpenDir*>(g_dir_mem); }

void InitOnce()
{
	std::call_once(g_once, [] {
		for (int i = 0; i < kMaxFiles; i++)
			new (&Files()[i]) OpenFile;
		for (int i = 0; i < kMaxDirs; i++)
			new (&Dirs()[i]) OpenDir;
	});
}

bool InRange(const void* p, const unsigned char* mem, size_t bytes, size_t stride)
{
	uintptr_t a = (uintptr_t)p, b = (uintptr_t)mem;
	return a >= b && a < b + bytes && (a - b) % stride == 0;
}

OpenFile* Claim()
{
	InitOnce();
	std::lock_guard<std::mutex> lk(g_slots_mu);
	for (int i = 0; i < kMaxFiles; i++)
		if (!Files()[i].used)
		{
			OpenFile& f = Files()[i];
			f.used = true;
			f.share.reset();
			f.data.reset();
			f.size = f.pos = 0;
			f.eof = f.err = false;
			f.pushback = -1;
			return &f;
		}
	return nullptr;
}

// The slot behind a FILE* or descriptor, or null (with EBADF) when it was closed.
OpenFile* FileOf(const FILE* f)
{
	OpenFile* of = (OpenFile*)f;
	if (!of->used)
	{
		errno = EBADF;
		return nullptr;
	}
	return of;
}

OpenFile* FileOfFd(int fd)
{
	if (!IsFd(fd))
	{
		errno = EBADF;
		return nullptr;
	}
	return FileOf(reinterpret_cast<FILE*>(&Files()[fd - kFdBase]));
}

int ErrnoOf(const Status& s)
{
	return s.err ? s.err : EIO;
}

void FillStat(struct stat* st, const Attr& a)
{
	memset(st, 0, sizeof *st);
	st->st_mode = a.is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
	st->st_nlink = 1;
	st->st_size = (off_t)a.size;
	st->st_blksize = 4096;
	st->st_blocks = (blkcnt_t)((a.size + 511) / 512);
	st->st_atime = st->st_mtime = st->st_ctime = (time_t)a.mtime;
}

// Reads up to n bytes at the file's position. Sets eof at the end of the file and err on a failed read.
size_t ReadBytes(OpenFile& f, uint8_t* dst, size_t n)
{
	size_t done = 0;
	if (n && f.pushback >= 0)
	{
		dst[done++] = (uint8_t)f.pushback;
		f.pushback = -1;
	}
	while (done < n)
	{
		if (f.pos >= f.size)
		{
			f.eof = true;
			break;
		}
		int64_t r = f.data->ReadAt(f.pos, dst + done, n - done);
		if (r < 0)
		{
			f.err = true;
			errno = (int)-r;
			break;
		}
		if (r == 0)
		{
			f.eof = true; // the file is shorter than it said
			break;
		}
		f.pos += (uint64_t)r;
		done += (size_t)r;
	}
	return done;
}

int Reject(int err)
{
	errno = err;
	return -1;
}

// Looks the path up on its share. Handled (true): `ret` is 0 or -1 with errno set.
bool Lookup(const char* path, Route& r, Attr& a, int& ret)
{
	if (!Resolve(path, r))
		return false;
	Status s = r.share->cache().Stat(r.rel, a);
	if (!s.ok())
	{
		errno = ErrnoOf(s);
		ret = -1;
		return true;
	}
	ret = 0;
	return true;
}
} // namespace

bool IsFile(const FILE* f)
{
	return InRange(f, g_file_mem, sizeof g_file_mem, sizeof(OpenFile));
}

bool IsFd(int fd)
{
	return fd >= kFdBase && fd < kFdBase + kMaxFiles;
}

bool IsDir(const DIR* d)
{
	return InRange(d, g_dir_mem, sizeof g_dir_mem, sizeof(OpenDir));
}

// ---- paths ----

namespace
{
// Opens a regular file for reading. Returns the slot, or null with errno set; false: not a network path.
bool OpenSlot(const char* path, OpenFile*& slot)
{
	Route r;
	if (!Resolve(path, r))
		return false;
	slot = nullptr;
	std::shared_ptr<FileData> data;
	Status s = r.share->cache().Open(r.rel, data);
	if (!s.ok())
	{
		errno = ErrnoOf(s);
		return true;
	}
	OpenFile* f = Claim();
	if (!f)
	{
		errno = EMFILE;
		return true;
	}
	f->share = r.share;
	f->size = data->size;
	f->data = std::move(data);
	slot = f;
	return true;
}
} // namespace

bool Fopen(const char* path, const char* mode, FILE*& ret)
{
	Route probe;
	if (!Resolve(path, probe))
		return false;
	if (!mode || mode[0] != 'r' || strchr(mode, '+'))
	{
		errno = EROFS;
		ret = nullptr;
		return true;
	}
	OpenFile* f = nullptr;
	OpenSlot(path, f);
	ret = reinterpret_cast<FILE*>(f);
	return true;
}

bool Open(const char* path, int flags, int& ret)
{
	Route probe;
	if (!Resolve(path, probe))
		return false;
	if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
	{
		ret = Reject(EROFS);
		return true;
	}
	OpenFile* f = nullptr;
	OpenSlot(path, f);
	ret = f ? kFdBase + (int)(f - Files()) : -1;
	return true;
}

bool Stat(const char* path, struct stat* st, int& ret)
{
	Route r;
	Attr a;
	if (!Lookup(path, r, a, ret))
		return false;
	if (ret == 0)
		FillStat(st, a);
	return true;
}

bool Access(const char* path, int mode, int& ret)
{
	Route r;
	Attr a;
	if (!Lookup(path, r, a, ret))
		return false;
	if (ret == 0 && (mode & W_OK))
		ret = Reject(EROFS);
	return true;
}

bool Realpath(const char* path, char* resolved, char*& ret)
{
	Route r;
	Attr a;
	int rc = 0;
	if (!Lookup(path, r, a, rc))
		return false;
	ret = nullptr;
	if (rc != 0)
		return true;
	std::string canon = r.share->mount_point + (r.rel.empty() ? "" : "/" + r.rel);
	if (resolved)
	{
		if (canon.size() >= PATH_MAX)
			errno = ENAMETOOLONG;
		else
		{
			memcpy(resolved, canon.c_str(), canon.size() + 1);
			ret = resolved;
		}
	}
	else
		ret = strdup(canon.c_str());
	return true;
}

bool Opendir(const char* path, DIR*& ret)
{
	Route r;
	Attr a;
	int rc = 0;
	if (!Lookup(path, r, a, rc))
		return false;
	ret = nullptr;
	if (rc != 0)
		return true;
	if (!a.is_dir)
	{
		errno = ENOTDIR;
		return true;
	}
	std::vector<DirEntry> list;
	Status s = r.share->cache().List(r.rel, list);
	if (!s.ok())
	{
		errno = ErrnoOf(s);
		return true;
	}
	InitOnce();
	std::lock_guard<std::mutex> lk(g_slots_mu);
	for (int i = 0; i < kMaxDirs; i++)
		if (!Dirs()[i].used)
		{
			OpenDir& d = Dirs()[i];
			d.used = true;
			d.next = 0;
			d.entries.clear();
			DirEntry dot;
			dot.attr.is_dir = true;
			dot.name = ".";
			d.entries.push_back(dot);
			dot.name = "..";
			d.entries.push_back(dot);
			for (DirEntry& e : list)
				d.entries.push_back(std::move(e));
			ret = reinterpret_cast<DIR*>(&d);
			return true;
		}
	errno = EMFILE;
	return true;
}

// ---- FILE* ----

int Fclose(FILE* fp)
{
	OpenFile* f = FileOf(fp);
	if (!f)
		return EOF;
	{
		std::lock_guard<std::recursive_mutex> lk(f->mu);
		f->data.reset(); // the last reference closes the server's handle
		f->share.reset();
	}
	std::lock_guard<std::mutex> lk(g_slots_mu);
	f->used = false;
	return 0;
}

size_t Fread(void* p, size_t size, size_t n, FILE* fp)
{
	OpenFile* f = FileOf(fp);
	if (!f || !size || !n)
		return 0;
	if (n > SIZE_MAX / size)
	{
		errno = EOVERFLOW;
		return 0;
	}
	std::lock_guard<std::recursive_mutex> lk(f->mu);
	return ReadBytes(*f, (uint8_t*)p, size * n) / size;
}

int Fseek(FILE* fp, long long off, int whence)
{
	OpenFile* f = FileOf(fp);
	if (!f)
		return -1;
	std::lock_guard<std::recursive_mutex> lk(f->mu);
	long long base;
	switch (whence)
	{
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = (long long)f->pos - (f->pushback >= 0 ? 1 : 0);
		break;
	case SEEK_END:
		base = (long long)f->size;
		break;
	default:
		return Reject(EINVAL);
	}
	if (off < -base)
		return Reject(EINVAL);
	if (off > 0 && base > LLONG_MAX - off)
		return Reject(EOVERFLOW);
	f->pos = (uint64_t)(base + off);
	f->eof = false;
	f->pushback = -1;
	return 0;
}

long long Ftell(FILE* fp)
{
	OpenFile* f = FileOf(fp);
	if (!f)
		return -1;
	std::lock_guard<std::recursive_mutex> lk(f->mu);
	return (long long)f->pos - (f->pushback >= 0 ? 1 : 0);
}

int Feof(FILE* fp)
{
	OpenFile* f = FileOf(fp);
	return f && f->eof;
}

int Ferror(FILE* fp)
{
	OpenFile* f = FileOf(fp);
	return f && f->err;
}

void Clearerr(FILE* fp)
{
	if (OpenFile* f = FileOf(fp))
	{
		std::lock_guard<std::recursive_mutex> lk(f->mu);
		f->eof = f->err = false;
	}
}

int Fileno(FILE* fp)
{
	OpenFile* f = FileOf(fp);
	return f ? kFdBase + (int)(f - Files()) : -1;
}

int Fgetc(FILE* fp)
{
	OpenFile* f = FileOf(fp);
	if (!f)
		return EOF;
	std::lock_guard<std::recursive_mutex> lk(f->mu);
	uint8_t c;
	return ReadBytes(*f, &c, 1) == 1 ? c : EOF;
}

int Ungetc(int c, FILE* fp)
{
	OpenFile* f = FileOf(fp);
	if (!f || c == EOF)
		return EOF;
	std::lock_guard<std::recursive_mutex> lk(f->mu);
	if (f->pushback >= 0)
		return EOF; // one character, as the C standard guarantees
	f->pushback = (uint8_t)c;
	f->eof = false;
	return (uint8_t)c;
}

char* Fgets(char* s, int n, FILE* fp)
{
	OpenFile* f = FileOf(fp);
	if (!f || n <= 0)
		return nullptr;
	std::lock_guard<std::recursive_mutex> lk(f->mu);
	int i = 0;
	while (i < n - 1)
	{
		uint8_t c;
		if (ReadBytes(*f, &c, 1) != 1)
			break;
		s[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (i == 0 && n > 1)
		return nullptr;
	s[i] = 0;
	return s;
}

void Flockfile(FILE* fp)
{
	if (OpenFile* f = FileOf(fp))
		f->mu.lock();
}

void Funlockfile(FILE* fp)
{
	if (OpenFile* f = FileOf(fp))
		f->mu.unlock();
}

// ---- file descriptors ----

int Close(int fd)
{
	OpenFile* f = FileOfFd(fd);
	return f ? (Fclose(reinterpret_cast<FILE*>(f)) == 0 ? 0 : -1) : -1;
}

long Read(int fd, void* p, size_t n)
{
	OpenFile* f = FileOfFd(fd);
	if (!f)
		return -1;
	std::lock_guard<std::recursive_mutex> lk(f->mu);
	f->err = false;
	size_t got = ReadBytes(*f, (uint8_t*)p, n);
	if (f->err && got == 0)
		return -1; // errno was set by ReadBytes
	return (long)got;
}

long Pread(int fd, void* p, size_t n, long long off)
{
	OpenFile* f = FileOfFd(fd);
	if (!f)
		return -1;
	if (off < 0)
		return Reject(EINVAL);
	int64_t r = f->data->ReadAt((uint64_t)off, p, n);
	if (r < 0)
		return Reject((int)-r);
	return (long)r;
}

long long Lseek(int fd, long long off, int whence)
{
	OpenFile* f = FileOfFd(fd);
	if (!f || Fseek(reinterpret_cast<FILE*>(f), off, whence) != 0)
		return -1;
	return Ftell(reinterpret_cast<FILE*>(f));
}

int Fstat(int fd, struct stat* st)
{
	OpenFile* f = FileOfFd(fd);
	if (!f)
		return -1;
	Attr a;
	a.size = f->size;
	FillStat(st, a);
	return 0;
}

// ---- DIR* ----

struct dirent* Readdir(DIR* dp)
{
	OpenDir* d = (OpenDir*)dp;
	if (!d->used)
	{
		errno = EBADF;
		return nullptr;
	}
	while (d->next < d->entries.size())
	{
		const DirEntry& e = d->entries[d->next++];
		if (e.name.size() >= sizeof d->cur.d_name)
			continue; // a name this struct cannot hold
		memset(&d->cur, 0, sizeof d->cur);
		memcpy(d->cur.d_name, e.name.c_str(), e.name.size() + 1);
		d->cur.d_type = e.attr.is_dir ? DT_DIR : DT_REG;
		return &d->cur;
	}
	return nullptr;
}

int Closedir(DIR* dp)
{
	OpenDir* d = (OpenDir*)dp;
	if (!d->used)
		return Reject(EBADF);
	d->entries.clear();
	d->entries.shrink_to_fit();
	std::lock_guard<std::mutex> lk(g_slots_mu);
	d->used = false;
	return 0;
}
} // namespace vfs
} // namespace netfs
