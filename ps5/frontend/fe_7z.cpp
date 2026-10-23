// Snes9x PS5 frontend: 7-Zip archives (fe_7z.h), over the LZMA SDK's 7z decoder.
// SPDX-License-Identifier: MIT

#include "fe_7z.h"

// the vendored SDK's symbols carry an s9x7z_ prefix (ps5/third_party/lzma7z/s9x7z_prefix.h): first, before its headers
#include "s9x7z_prefix.h"
#include "7z.h"
#include "7zCrc.h"

#include <strings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/types.h>

namespace fe
{
namespace sevenzip
{
namespace
{
constexpr uint32_t kAesMethod = 0x06F10701; // 7zAES: an archive with a password
constexpr size_t kLookBuf = 1 << 18; // read ahead, as the SDK's own 7zMain does

// ISeekInStream over a FILE* (the libc calls, so netfs answers for a share)
struct FileStream
{
	ISeekInStream vt;
	FILE* f;
};

SRes FileRead(ISeekInStreamPtr pp, void* buf, size_t* size)
{
	FILE* f = reinterpret_cast<const FileStream*>(pp)->f;
	const size_t want = *size;
	*size = want ? fread(buf, 1, want, f) : 0;
	return *size == 0 && want && ferror(f) ? SZ_ERROR_READ : SZ_OK;
}

SRes FileSeek(ISeekInStreamPtr pp, Int64* pos, ESzSeek origin)
{
	FILE* f = reinterpret_cast<const FileStream*>(pp)->f;
	const int whence = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
	if (fseeko(f, off_t(*pos), whence) != 0)
		return SZ_ERROR_READ;
	const off_t at = ftello(f);
	if (at < 0)
		return SZ_ERROR_READ;
	*pos = Int64(at);
	return SZ_OK;
}

// An allocator with a budget: what the decoder asks for past it fails (SZ_ERROR_MEM), it never takes more.
struct CapAlloc
{
	ISzAlloc vt;
	uint64_t used;
	uint64_t cap;
};

constexpr size_t kAllocHead = 16; // the size, kept in front (malloc's 16-byte alignment is kept)

void* CapAllocFn(ISzAllocPtr pp, size_t size)
{
	CapAlloc* p = const_cast<CapAlloc*>(reinterpret_cast<const CapAlloc*>(pp));
	if (size == 0 || size > p->cap - p->used || size > SIZE_MAX - kAllocHead)
		return nullptr;
	void* m = malloc(size + kAllocHead);
	if (!m)
		return nullptr;
	*static_cast<size_t*>(m) = size;
	p->used += size;
	return static_cast<char*>(m) + kAllocHead;
}

void CapFreeFn(ISzAllocPtr pp, void* a)
{
	if (!a)
		return;
	CapAlloc* p = const_cast<CapAlloc*>(reinterpret_cast<const CapAlloc*>(pp));
	char* base = static_cast<char*>(a) - kAllocHead;
	p->used -= *reinterpret_cast<size_t*>(base);
	free(base);
}

uint64_t Le64(const uint8_t* b)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | b[i];
	return v;
}

// An archive whose header the SDK can't open (SZ_ERROR_UNSUPPORTED): is it because the header is encrypted (7z a -mhe)?
// The encoded header's own coder list is plain: 7zAES's method ID there, after a coder byte with a 4-byte ID.
bool HeaderEncrypted(FILE* f)
{
	uint8_t start[32];
	if (fseeko(f, 0, SEEK_SET) != 0 || fread(start, 1, sizeof(start), f) != sizeof(start))
		return false;
	const uint64_t off = Le64(start + 12), size = Le64(start + 20);
	if (size < 5 || size > (1u << 20) || off > (uint64_t(1) << 40))
		return false;
	std::vector<uint8_t> head(size);
	if (fseeko(f, off_t(32 + off), SEEK_SET) != 0 || fread(head.data(), 1, head.size(), f) != head.size())
		return false;
	static const uint8_t aes[4] = {0x06, 0xF1, 0x07, 0x01};
	for (size_t i = 1; i + 4 <= head.size(); i++)
		if ((head[i - 1] & 0x0F) == 4 && memcmp(&head[i], aes, 4) == 0)
			return true;
	return false;
}

bool FolderHasAes(const CSzAr* ar, uint32_t folder)
{
	CSzData sd;
	sd.Data = ar->CodersData + ar->FoCodersOffsets[folder];
	sd.Size = ar->FoCodersOffsets[folder + 1] - ar->FoCodersOffsets[folder];
	CSzFolder fo;
	if (SzGetNextFolderItem(&fo, &sd) != SZ_OK)
		return false;
	for (uint32_t i = 0; i < fo.NumCoders; i++)
		if (fo.Coders[i].MethodID == kAesMethod)
			return true;
	return false;
}

Error FromSRes(SRes r)
{
	switch (r)
	{
		case SZ_OK: return Error::None;
		case SZ_ERROR_MEM: return Error::Memory;
		case SZ_ERROR_UNSUPPORTED: return Error::Unsupported;
		case SZ_ERROR_READ: return Error::Open;
		default: return Error::Damaged; // DATA, CRC, ARCHIVE, NO_ARCHIVE, INPUT_EOF, FAIL...
	}
}

void AppendUtf8(std::string& out, uint32_t c)
{
	if (c < 0x80)
		out += char(c);
	else if (c < 0x800)
	{
		out += char(0xC0 | (c >> 6));
		out += char(0x80 | (c & 0x3F));
	}
	else if (c < 0x10000)
	{
		out += char(0xE0 | (c >> 12));
		out += char(0x80 | ((c >> 6) & 0x3F));
		out += char(0x80 | (c & 0x3F));
	}
	else
	{
		out += char(0xF0 | (c >> 18));
		out += char(0x80 | ((c >> 12) & 0x3F));
		out += char(0x80 | ((c >> 6) & 0x3F));
		out += char(0x80 | (c & 0x3F));
	}
}

struct Archive
{
	FILE* f = nullptr;
	FileStream file{};
	CLookToRead2 look{};
	CSzArEx db{};
	CapAlloc main{};
	CapAlloc temp{};
	std::vector<Byte> lookbuf;
	bool inited = false;

	~Archive()
	{
		if (inited)
			SzArEx_Free(&db, &main.vt);
		if (f)
			fclose(f);
	}

	Error Open(const std::string& path)
	{
		static std::once_flag crc_once;
		std::call_once(crc_once, [] { CrcGenerateTable(); });
		f = fopen(path.c_str(), "rb");
		if (!f)
			return Error::Open;
		file.vt.Read = FileRead;
		file.vt.Seek = FileSeek;
		file.f = f;
		lookbuf.resize(kLookBuf);
		LookToRead2_CreateVTable(&look, False);
		look.buf = lookbuf.data();
		look.bufSize = lookbuf.size();
		look.realStream = &file.vt;
		LookToRead2_INIT(&look);
		main.vt.Alloc = temp.vt.Alloc = CapAllocFn;
		main.vt.Free = temp.vt.Free = CapFreeFn;
		main.cap = kMaxBlock + kMaxMemory;
		temp.cap = kMaxMemory;
		SzArEx_Init(&db);
		inited = true;
		const SRes r = SzArEx_Open(&db, &look.vt, &main.vt, &temp.vt);
		if (r == SZ_ERROR_UNSUPPORTED && HeaderEncrypted(f))
			return Error::Encrypted;
		return FromSRes(r);
	}

	std::string Name(uint32_t i) const
	{
		const size_t n = SzArEx_GetFileNameUtf16(&db, i, nullptr);
		std::vector<UInt16> w(n + 1, 0);
		SzArEx_GetFileNameUtf16(&db, i, w.data());
		std::string out;
		for (size_t k = 0; k < n && w[k]; k++)
		{
			uint32_t c = w[k];
			if (c >= 0xD800 && c < 0xDC00 && k + 1 < n && w[k + 1] >= 0xDC00 && w[k + 1] < 0xE000)
				c = 0x10000 + ((c - 0xD800) << 10) + (w[++k] - 0xDC00);
			AppendUtf8(out, c);
		}
		return out;
	}
};

// 1: a disc's sheet (the disc's other files may have any name, .bin included); 2: a disc image on its own; 0: neither
int DiscKind(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos)
		return 0;
	static const char* const sheets[] = {".cue", ".ccd", ".gdi", ".toc", ".m3u", ".mds"};
	static const char* const images[] = {".iso", ".chd", ".cdi", ".img", ".mdf", ".pbp", ".cso"};
	for (const char* e : sheets)
		if (strcasecmp(name.c_str() + dot, e) == 0)
			return 1;
	for (const char* e : images)
		if (strcasecmp(name.c_str() + dot, e) == 0)
			return 2;
	return 0;
}

thread_local Error g_last_load = Error::None;
} // namespace

bool IsArchivePath(const std::string& path)
{
	return path.size() >= 3 && strcasecmp(path.c_str() + path.size() - 3, ".7z") == 0;
}

bool List(const std::string& path, std::vector<Entry>* entries, Error* err)
{
	entries->clear();
	Archive a;
	*err = a.Open(path);
	if (*err != Error::None)
		return false;
	for (uint32_t i = 0; i < a.db.NumFiles; i++)
	{
		Entry e;
		e.index = i;
		e.name = a.Name(i);
		e.is_dir = SzArEx_IsDir(&a.db, i);
		e.size = SzArEx_GetFileSize(&a.db, i);
		e.has_crc = SzBitWithVals_Check(&a.db.CRCs, i);
		e.crc = e.has_crc ? a.db.CRCs.Vals[i] : 0;
		const uint32_t folder = a.db.FileToFolder[i];
		e.block_size = folder == uint32_t(-1) ? 0 : SzAr_GetFolderUnpackSize(&a.db.db, folder);
		entries->push_back(std::move(e));
	}
	return true;
}

int PickRom(const std::vector<Entry>& entries, uint64_t max_size, Error* err)
{
	int best = -1;
	bool too_big = false, disc = false;
	for (const Entry& e : entries)
		if (!e.is_dir && DiscKind(e.name) == 1)
		{
			*err = Error::DiscImage;
			return -1;
		}
	for (size_t i = 0; i < entries.size(); i++)
	{
		const Entry& e = entries[i];
		if (e.is_dir || e.size == 0)
			continue;
		if (DiscKind(e.name))
		{
			disc = true;
			continue;
		}
		if (e.size > max_size)
		{
			too_big = true;
			continue;
		}
		if (best < 0 || e.size > entries[size_t(best)].size)
			best = int(i);
	}
	*err = best >= 0 ? Error::None : too_big ? Error::TooBig : disc ? Error::DiscImage : Error::NoRom;
	return best;
}

bool Extract(const std::string& path, uint32_t index, std::vector<uint8_t>* out, Error* err)
{
	out->clear();
	Archive a;
	*err = a.Open(path);
	if (*err != Error::None)
		return false;
	if (index >= a.db.NumFiles || SzArEx_IsDir(&a.db, index))
	{
		*err = Error::NoRom;
		return false;
	}
	const uint32_t folder = a.db.FileToFolder[index];
	if (folder == uint32_t(-1))
		return true; // an empty file
	if (SzAr_GetFolderUnpackSize(&a.db.db, folder) > kMaxBlock)
	{
		*err = Error::BlockTooBig;
		return false;
	}
	if (FolderHasAes(&a.db.db, folder))
	{
		*err = Error::Encrypted;
		return false;
	}
	UInt32 block = 0xFFFFFFFF;
	Byte* buf = nullptr;
	size_t buf_size = 0, offset = 0, size = 0;
	const SRes r = SzArEx_Extract(&a.db, &a.look.vt, index, &block, &buf, &buf_size, &offset, &size, &a.main.vt, &a.temp.vt);
	if (r == SZ_OK)
		out->assign(buf + offset, buf + offset + size);
	ISzAlloc_Free(&a.main.vt, buf);
	*err = FromSRes(r);
	return r == SZ_OK;
}

bool ExtractRom(const std::string& path, uint64_t max_size, std::vector<uint8_t>* out, Error* err, std::string* name)
{
	out->clear();
	std::vector<Entry> entries;
	if (!List(path, &entries, err))
		return false;
	const int k = PickRom(entries, max_size, err);
	if (k < 0)
		return false;
	if (name)
		*name = entries[size_t(k)].name;
	return Extract(path, entries[size_t(k)].index, out, err);
}

const char* Message(Error err)
{
	switch (err)
	{
		case Error::None: return "";
		case Error::Open: return "The 7z archive could not be read.";
		case Error::Damaged: return "The 7z archive is damaged or is not a 7z archive.";
		case Error::Encrypted: return "The 7z archive has a password: encrypted archives are not supported.";
		case Error::Unsupported: return "The 7z archive uses a compression method that is not supported.";
		case Error::NoRom: return "The 7z archive holds no SNES ROM.";
		case Error::DiscImage: return "The 7z archive holds a CD image: CD images in .7z archives are not supported.";
		case Error::TooBig: return "The ROM in the 7z archive is too big (over 16 MiB).";
		case Error::BlockTooBig: return "The 7z archive is solid with a block over 64 MiB: pack the ROM in an archive of its own.";
		case Error::Memory: return "Not enough memory to unpack the 7z archive.";
	}
	return "";
}

Error LastLoadError()
{
	return g_last_load;
}

void SetLastLoadError(Error err)
{
	g_last_load = err;
}
} // namespace sevenzip
} // namespace fe
