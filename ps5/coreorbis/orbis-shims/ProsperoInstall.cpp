// Snes9x PS5: installs the dashboard app from files built into the payload (ProsperoInstall.h).
//
// Like PS5SX2's installer: a file is only rewritten when its bytes differ, each one is written to
// <name>.part, flushed and then renamed over the old one (a power cut leaves either the old or the new file),
// and eboot.bin goes last, so a half-done update never has a new eboot with old metadata. Nothing outside
// the app folder is touched.
//
// SPDX-License-Identifier: MIT

#include "ProsperoInstall.h"

#include "OrbisPaths.h"
#include "ProsperoUpdate.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Overridden by install_data.cpp in the build that carries the app.
__attribute__((weak)) const EmbeddedAppFile* EmbeddedAppFiles()
{
	return nullptr;
}

namespace
{
bool SameContent(const std::string& path, const unsigned char* data, size_t size)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || size_t(st.st_size) != size)
		return false;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<unsigned char> buf(1 << 16);
	size_t off = 0;
	bool same = true;
	while (same && off < size)
	{
		const size_t n = fread(buf.data(), 1, buf.size(), f);
		if (n == 0)
			break;
		same = memcmp(buf.data(), data + off, n) == 0;
		off += n;
	}
	fclose(f);
	return same && off == size;
}

bool WriteAtomically(const std::string& path, const unsigned char* data, size_t size)
{
	const std::string part = path + ".part";
	const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
	{
		OrbisLog("[install] can't create %s", part.c_str());
		return false;
	}
	size_t off = 0;
	while (off < size)
	{
		const ssize_t n = write(fd, data + off, size - off);
		if (n <= 0)
		{
			close(fd);
			unlink(part.c_str());
			OrbisLog("[install] write failed: %s (disk full?)", part.c_str());
			return false;
		}
		off += size_t(n);
	}
	const bool synced = fsync(fd) == 0;
	const bool closed = close(fd) == 0;
	if (!synced || !closed || rename(part.c_str(), path.c_str()) != 0)
	{
		unlink(part.c_str());
		OrbisLog("[install] can't finish %s", part.c_str());
		return false;
	}
	return true;
}
} // namespace

std::string AppInstallDir()
{
	const char* env = getenv("SNES9X_PS5_HOMEBREW");
	return std::string(env && *env ? env : "/data/homebrew") + "/" SNES9X_TITLE_ID;
}

namespace
{
std::string ContentVersion(const std::string& param)
{
	const size_t k = param.find("\"contentVersion\"");
	if (k == std::string::npos)
		return "";
	const size_t a = param.find('"', param.find(':', k) + 1);
	const size_t b = a == std::string::npos ? a : param.find('"', a + 1);
	return b == std::string::npos ? "" : param.substr(a + 1, b - a - 1);
}
} // namespace

std::string InstalledVersion()
{
	FILE* f = fopen((AppInstallDir() + "/sce_sys/param.json").c_str(), "rb");
	if (!f)
		return "";
	std::string s(65536, '\0');
	s.resize(fread(&s[0], 1, s.size(), f));
	fclose(f);
	return ContentVersion(s);
}

InstallResult InstallApp()
{
	const EmbeddedAppFile* files = EmbeddedAppFiles();
	if (!files)
		return InstallResult::NothingEmbedded;

	const std::string dir = AppInstallDir();
	// a newer version installed (by the app's own update): this older payload doesn't put itself over it
	for (const EmbeddedAppFile* f = files; f->rel; f++)
		if (strcmp(f->rel, "sce_sys/param.json") == 0)
		{
			const std::string mine = ContentVersion(std::string(reinterpret_cast<const char*>(f->begin), size_t(f->end - f->begin)));
			const std::string there = InstalledVersion();
			if (!mine.empty() && !there.empty() && update::CompareVersions(there, mine) > 0)
			{
				OrbisLog("[install] %s holds version %s, newer than this payload's %s: left as it is", dir.c_str(),
					there.c_str(), mine.c_str());
				return InstallResult::NewerInstalled;
			}
		}
	const bool existed = OrbisIsFile(dir + "/eboot.bin");
	if (!OrbisMkdirs(dir + "/sce_sys"))
	{
		OrbisLog("[install] can't create %s", dir.c_str());
		return InstallResult::Failed;
	}

	// eboot.bin last
	std::vector<const EmbeddedAppFile*> order;
	const EmbeddedAppFile* eboot = nullptr;
	for (const EmbeddedAppFile* f = files; f->rel; f++)
	{
		if (strcmp(f->rel, "eboot.bin") == 0)
			eboot = f;
		else
			order.push_back(f);
	}
	if (eboot)
		order.push_back(eboot);

	int written = 0;
	for (const EmbeddedAppFile* f : order)
	{
		const std::string path = dir + "/" + f->rel;
		const size_t size = size_t(f->end - f->begin);
		if (SameContent(path, f->begin, size))
			continue;
		const size_t slash = path.rfind('/');
		if (slash != std::string::npos && !OrbisMkdirs(path.substr(0, slash)))
		{
			OrbisLog("[install] can't create the folder of %s", path.c_str());
			return InstallResult::Failed;
		}
		if (!WriteAtomically(path, f->begin, size))
			return InstallResult::Failed;
		OrbisLog("[install] wrote %s (%zu bytes)", path.c_str(), size);
		written++;
	}
	if (written == 0)
	{
		OrbisLog("[install] %s is up to date", dir.c_str());
		return InstallResult::UpToDate;
	}
	return existed ? InstallResult::Updated : InstallResult::Installed;
}

std::string AppMetaDir()
{
	const char* env = getenv("SNES9X_PS5_APPMETA");
	return std::string(env && *env ? env : "/user/appmeta") + "/" SNES9X_TITLE_ID;
}

int SyncAppMeta()
{
	// ShadowMountPlus copies sce_sys's art (icon0.png, pic0/pic1, param.json) to /user/appmeta/<title> only when
	// it first registers a title (or when that folder is gone); the home screen reads it from there. So an update
	// of the art never reached the screen (1.8: the new pic0.dds stayed in sce_sys). Keep our own title's copy
	// current. Nothing else in /user/appmeta is touched.
	const std::string dir = AppMetaDir();
	if (!OrbisIsDir(dir))
	{
		OrbisLog("[appmeta] %s not there yet (ShadowMountPlus registers the title first)", dir.c_str());
		return 0;
	}
	const EmbeddedAppFile* files = EmbeddedAppFiles();
	if (!files)
		return 0;
	int changed = 0;
	for (const EmbeddedAppFile* f = files; f->rel; f++)
	{
		if (strncmp(f->rel, "sce_sys/", 8) != 0)
			continue;
		const std::string name = f->rel + 8;
		const std::string path = dir + "/" + name;
		const size_t size = size_t(f->end - f->begin);
		if (SameContent(path, f->begin, size))
			continue;
		if (!WriteAtomically(path, f->begin, size))
		{
			OrbisLog("[appmeta] can't write %s", path.c_str());
			continue;
		}
		OrbisLog("[appmeta] wrote %s (%zu bytes)", path.c_str(), size);
		changed++;
	}
	// The PNG backgrounds of 1.0-1.7 would hide the DDS ones: remove them (ours, in our title's folders only).
	for (const char* old : {"pic0.png", "pic1.png"})
		for (const std::string& d : {dir, AppInstallDir() + "/sce_sys"})
		{
			const std::string path = d + "/" + old;
			if (OrbisIsFile(path) && unlink(path.c_str()) == 0)
			{
				OrbisLog("[appmeta] removed the old %s", path.c_str());
				changed++;
			}
		}
	OrbisLog("[appmeta] %s: %d change(s)", dir.c_str(), changed);
	return changed;
}
