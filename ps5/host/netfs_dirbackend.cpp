// Snes9x PS5 host build: a "share" that is a folder on this machine, for the end-to-end tests. With
// SNES9X_HOST_NETFS_DIR=<folder> every smb:// and nfs:// address in shares.txt connects to that folder, so the
// real app (shelf, launch, MSU-1) runs over netfs without a server. SNES9X_HOST_NETFS_DELAY_MS adds that many
// milliseconds to every read, as a slow link would. Not part of the console build.
//
// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "netfs.h"

namespace
{
class DirBackend : public netfs::Backend
{
public:
	DirBackend(std::string root, int delay_ms) : m_root(std::move(root)), m_delay_ms(delay_ms) {}

	netfs::Status Connect(const netfs::ShareConfig&) override
	{
		struct stat st;
		if (stat(m_root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
			return netfs::Status::Kind(netfs::ErrKind::FolderNotFound);
		return netfs::Status::Ok();
	}

	void Disconnect() override {}

	netfs::Status Stat(const std::string& path, netfs::Attr& out) override
	{
		struct stat st;
		if (stat(Full(path).c_str(), &st) != 0)
			return netfs::Status::Errno(errno);
		Fill(st, out);
		return netfs::Status::Ok();
	}

	netfs::Status List(const std::string& path, std::vector<netfs::DirEntry>& out) override
	{
		const std::string dir = Full(path);
		DIR* d = opendir(dir.c_str());
		if (!d)
			return netfs::Status::Errno(errno);
		while (dirent* e = readdir(d))
		{
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			struct stat st;
			if (stat((dir + "/" + e->d_name).c_str(), &st) != 0)
				continue;
			netfs::DirEntry de;
			de.name = e->d_name;
			Fill(st, de.attr);
			out.push_back(de);
		}
		closedir(d);
		return netfs::Status::Ok();
	}

	netfs::Status Open(const std::string& path, netfs::Handle& out) override
	{
		int fd = open(Full(path).c_str(), O_RDONLY);
		if (fd < 0)
			return netfs::Status::Errno(errno);
		out = reinterpret_cast<netfs::Handle>((intptr_t)fd + 1);
		return netfs::Status::Ok();
	}

	int64_t Pread(netfs::Handle h, void* buf, size_t len, uint64_t offset) override
	{
		if (m_delay_ms > 0)
			usleep((useconds_t)m_delay_ms * 1000);
		ssize_t r = pread((int)((intptr_t)h - 1), buf, len, (off_t)offset);
		return r < 0 ? -(int64_t)errno : (int64_t)r;
	}

	void Close(netfs::Handle h) override { close((int)((intptr_t)h - 1)); }
	void Abort() override {}

private:
	std::string Full(const std::string& rel) const { return rel.empty() ? m_root : m_root + "/" + rel; }
	static void Fill(const struct stat& st, netfs::Attr& a)
	{
		a.is_dir = S_ISDIR(st.st_mode);
		a.size = a.is_dir ? 0 : (uint64_t)st.st_size;
		a.mtime = (int64_t)st.st_mtime;
	}

	std::string m_root;
	int m_delay_ms;
};

struct Install
{
	Install()
	{
		const char* dir = getenv("SNES9X_HOST_NETFS_DIR");
		if (!dir || !*dir)
			return;
		const std::string root = dir;
		const char* delay = getenv("SNES9X_HOST_NETFS_DELAY_MS");
		const int delay_ms = delay ? atoi(delay) : 0;
		auto make = [root, delay_ms]() -> std::unique_ptr<netfs::Backend> { return std::make_unique<DirBackend>(root, delay_ms); };
		netfs_set_backend_factory(netfs::Scheme::Smb, make);
		netfs_set_backend_factory(netfs::Scheme::Nfs, make);
	}
} g_install;
} // namespace
