// Snes9x PS5 netfs: the libc file calls, answered for paths under a mount point. Internal; netfs_wrap.cpp is
// the only caller.
//
// Path functions return true when the path is under a mount point (the call was handled: `ret` and errno are
// set as the libc call would) and false when it is not (the wrapper then makes the real call). Handle
// functions are only called after IsFile / IsFd / IsDir said the handle is netfs's.
//
// Handles are slots in static tables: a FILE* or DIR* is a slot's address, a file descriptor is kFdBase plus the
// slot's index, so the check is a range test with no lock. Files are read-only and use a small read buffer.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>

namespace netfs
{
namespace vfs
{
constexpr int kFdBase = 0x40000000; // above any real descriptor
constexpr int kMaxFiles = 64;
constexpr int kMaxDirs = 16;

bool IsFile(const FILE* f);
bool IsFd(int fd);
bool IsDir(const DIR* d);

// ---- paths ----
bool Fopen(const char* path, const char* mode, FILE*& ret);
bool Open(const char* path, int flags, int& ret);
bool Stat(const char* path, struct stat* st, int& ret);
bool Access(const char* path, int mode, int& ret);
bool Realpath(const char* path, char* resolved, char*& ret);
bool Opendir(const char* path, DIR*& ret);

// ---- FILE* ----
int Fclose(FILE* f);
size_t Fread(void* p, size_t size, size_t n, FILE* f);
int Fseek(FILE* f, long long off, int whence);
long long Ftell(FILE* f);
int Feof(FILE* f);
int Ferror(FILE* f);
void Clearerr(FILE* f);
int Fileno(FILE* f);
int Fgetc(FILE* f);
int Ungetc(int c, FILE* f);
char* Fgets(char* s, int n, FILE* f);
void Flockfile(FILE* f);
void Funlockfile(FILE* f);

// ---- file descriptors ----
int Close(int fd);
long Read(int fd, void* p, size_t n);
long Pread(int fd, void* p, size_t n, long long off);
long long Lseek(int fd, long long off, int whence);
int Fstat(int fd, struct stat* st);

// ---- DIR* ----
struct dirent* Readdir(DIR* d);
int Closedir(DIR* d);
} // namespace vfs
} // namespace netfs
