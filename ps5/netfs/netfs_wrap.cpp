// Snes9x PS5 netfs: the linker's --wrap entry points.
//
// The EBOOT and host links pass --wrap=<name> for every function below (NETFS_WRAPPED in the Makefile: keep
// the two lists equal; tests/test_netfs.cpp calls each one on a network path). The linker sends every call to
// <name> in the code it links (the Snes9x core, minizip, the frontend, libc++'s file streams) to __wrap_<name>.
// A path that is not under a mount point, and a handle that is not netfs's, goes on to __real_<name>, the
// original: all the cost is a prefix test or a range test.
//
// A FILE* function that is not in this list must never meet a netfs FILE*. Read-only code that opens network
// paths can use only these. (fscanf, getline and the writers are not here: nothing reads ROMs with them.)
//
// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "netfs.h"
#include "netfs_vfs.h"

using namespace netfs;

extern "C"
{
FILE* __real_fopen(const char*, const char*);
FILE* __real_fopen64(const char*, const char*);
int __real_fclose(FILE*);
size_t __real_fread(void*, size_t, size_t, FILE*);
int __real_fseek(FILE*, long, int);
int __real_fseeko(FILE*, off_t, int);
int __real_fseeko64(FILE*, int64_t, int);
long __real_ftell(FILE*);
off_t __real_ftello(FILE*);
int64_t __real_ftello64(FILE*);
void __real_rewind(FILE*);
int __real_feof(FILE*);
int __real_ferror(FILE*);
void __real_clearerr(FILE*);
int __real_fileno(FILE*);
int __real_fgetc(FILE*);
int __real_getc(FILE*);
int __real_ungetc(int, FILE*);
char* __real_fgets(char*, int, FILE*);
int __real_fflush(FILE*);
int __real_setvbuf(FILE*, char*, int, size_t);
void __real_flockfile(FILE*);
void __real_funlockfile(FILE*);
int __real_open(const char*, int, ...);
int __real_close(int);
ssize_t __real_read(int, void*, size_t);
ssize_t __real_pread(int, void*, size_t, off_t);
off_t __real_lseek(int, off_t, int);
int __real_fstat(int, struct stat*);
int __real_stat(const char*, struct stat*);
int __real_lstat(const char*, struct stat*);
int __real_access(const char*, int);
char* __real_realpath(const char*, char*);
DIR* __real_opendir(const char*);
struct dirent* __real_readdir(DIR*);
int __real_closedir(DIR*);

// ---- by path ----

FILE* __wrap_fopen(const char* path, const char* mode)
{
	FILE* f;
	if (netfs_is_network_path(path) && vfs::Fopen(path, mode, f))
		return f;
	return __real_fopen(path, mode);
}

FILE* __wrap_fopen64(const char* path, const char* mode)
{
	FILE* f;
	if (netfs_is_network_path(path) && vfs::Fopen(path, mode, f))
		return f;
	return __real_fopen64(path, mode);
}

int __wrap_open(const char* path, int flags, ...)
{
	int fd;
	if (netfs_is_network_path(path) && vfs::Open(path, flags, fd))
		return fd;
	int mode = 0;
	if (flags & O_CREAT)
	{
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return __real_open(path, flags, mode);
}

int __wrap_stat(const char* path, struct stat* st)
{
	int rc;
	if (netfs_is_network_path(path) && vfs::Stat(path, st, rc))
		return rc;
	return __real_stat(path, st);
}

int __wrap_lstat(const char* path, struct stat* st)
{
	int rc;
	if (netfs_is_network_path(path) && vfs::Stat(path, st, rc))
		return rc;
	return __real_lstat(path, st);
}

int __wrap_access(const char* path, int mode)
{
	int rc;
	if (netfs_is_network_path(path) && vfs::Access(path, mode, rc))
		return rc;
	return __real_access(path, mode);
}

char* __wrap_realpath(const char* path, char* resolved)
{
	char* r;
	if (netfs_is_network_path(path) && vfs::Realpath(path, resolved, r))
		return r;
	return __real_realpath(path, resolved);
}

DIR* __wrap_opendir(const char* path)
{
	DIR* d;
	if (netfs_is_network_path(path) && vfs::Opendir(path, d))
		return d;
	return __real_opendir(path);
}

// ---- FILE* ----

int __wrap_fclose(FILE* f)
{
	return vfs::IsFile(f) ? vfs::Fclose(f) : __real_fclose(f);
}

size_t __wrap_fread(void* p, size_t size, size_t n, FILE* f)
{
	return vfs::IsFile(f) ? vfs::Fread(p, size, n, f) : __real_fread(p, size, n, f);
}

int __wrap_fseek(FILE* f, long off, int whence)
{
	return vfs::IsFile(f) ? vfs::Fseek(f, off, whence) : __real_fseek(f, off, whence);
}

int __wrap_fseeko(FILE* f, off_t off, int whence)
{
	return vfs::IsFile(f) ? vfs::Fseek(f, off, whence) : __real_fseeko(f, off, whence);
}

int __wrap_fseeko64(FILE* f, int64_t off, int whence)
{
	return vfs::IsFile(f) ? vfs::Fseek(f, off, whence) : __real_fseeko64(f, off, whence);
}

long __wrap_ftell(FILE* f)
{
	return vfs::IsFile(f) ? (long)vfs::Ftell(f) : __real_ftell(f);
}

off_t __wrap_ftello(FILE* f)
{
	return vfs::IsFile(f) ? (off_t)vfs::Ftell(f) : __real_ftello(f);
}

int64_t __wrap_ftello64(FILE* f)
{
	return vfs::IsFile(f) ? (int64_t)vfs::Ftell(f) : __real_ftello64(f);
}

void __wrap_rewind(FILE* f)
{
	if (vfs::IsFile(f))
	{
		vfs::Fseek(f, 0, SEEK_SET);
		vfs::Clearerr(f);
	}
	else
		__real_rewind(f);
}

int __wrap_feof(FILE* f)
{
	return vfs::IsFile(f) ? vfs::Feof(f) : __real_feof(f);
}

int __wrap_ferror(FILE* f)
{
	return vfs::IsFile(f) ? vfs::Ferror(f) : __real_ferror(f);
}

void __wrap_clearerr(FILE* f)
{
	if (vfs::IsFile(f))
		vfs::Clearerr(f);
	else
		__real_clearerr(f);
}

int __wrap_fileno(FILE* f)
{
	return vfs::IsFile(f) ? vfs::Fileno(f) : __real_fileno(f);
}

int __wrap_fgetc(FILE* f)
{
	return vfs::IsFile(f) ? vfs::Fgetc(f) : __real_fgetc(f);
}

int __wrap_getc(FILE* f)
{
	return vfs::IsFile(f) ? vfs::Fgetc(f) : __real_getc(f);
}

int __wrap_ungetc(int c, FILE* f)
{
	return vfs::IsFile(f) ? vfs::Ungetc(c, f) : __real_ungetc(c, f);
}

char* __wrap_fgets(char* s, int n, FILE* f)
{
	return vfs::IsFile(f) ? vfs::Fgets(s, n, f) : __real_fgets(s, n, f);
}

int __wrap_fflush(FILE* f)
{
	return vfs::IsFile(f) ? 0 : __real_fflush(f); // fflush(NULL) is the real call's
}

int __wrap_setvbuf(FILE* f, char* buf, int mode, size_t size)
{
	return vfs::IsFile(f) ? 0 : __real_setvbuf(f, buf, mode, size);
}

void __wrap_flockfile(FILE* f)
{
	if (vfs::IsFile(f))
		vfs::Flockfile(f);
	else
		__real_flockfile(f);
}

void __wrap_funlockfile(FILE* f)
{
	if (vfs::IsFile(f))
		vfs::Funlockfile(f);
	else
		__real_funlockfile(f);
}

// ---- file descriptors ----

int __wrap_close(int fd)
{
	return vfs::IsFd(fd) ? vfs::Close(fd) : __real_close(fd);
}

ssize_t __wrap_read(int fd, void* p, size_t n)
{
	return vfs::IsFd(fd) ? vfs::Read(fd, p, n) : __real_read(fd, p, n);
}

ssize_t __wrap_pread(int fd, void* p, size_t n, off_t off)
{
	return vfs::IsFd(fd) ? vfs::Pread(fd, p, n, off) : __real_pread(fd, p, n, off);
}

off_t __wrap_lseek(int fd, off_t off, int whence)
{
	return vfs::IsFd(fd) ? (off_t)vfs::Lseek(fd, off, whence) : __real_lseek(fd, off, whence);
}

int __wrap_fstat(int fd, struct stat* st)
{
	return vfs::IsFd(fd) ? vfs::Fstat(fd, st) : __real_fstat(fd, st);
}

// ---- DIR* ----

struct dirent* __wrap_readdir(DIR* d)
{
	return vfs::IsDir(d) ? vfs::Readdir(d) : __real_readdir(d);
}

int __wrap_closedir(DIR* d)
{
	return vfs::IsDir(d) ? vfs::Closedir(d) : __real_closedir(d);
}
} // extern "C"
