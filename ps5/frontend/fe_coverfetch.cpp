// Snes9x PS5: downloading covers (fe_coverfetch.h).
// SPDX-License-Identifier: MIT

#include "fe_coverfetch.h"

#include "OrbisPaths.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{
bool IsImage(const std::vector<uint8_t>& d)
{
	return (d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') ||
		   (d.size() > 3 && d[0] == 0xFF && d[1] == 0xD8);
}

bool WriteFileAtomicTo(const std::string& path, const std::vector<uint8_t>& data)
{
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size(); // an empty list is a valid file
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

namespace
{
int FetchOne(HttpClient& http, std::string url, const std::string& label, std::vector<uint8_t>& data)
{
	int status = http.Get(url, data);
	// libretro-thumbnails keeps many variants as git symlinks: the "image" is then the name of the real
	// file ("Donkey Kong Country (USA).png"), which sits in the same folder. Follow up to two of them.
	for (int hop = 0; hop < 2 && status == 200 && !IsImage(data); hop++)
	{
		std::string target(data.begin(), data.end());
		while (!target.empty() && (target.back() == '\n' || target.back() == '\r' || target.back() == ' '))
			target.pop_back();
		const size_t slash = target.find_last_of('/');
		if (slash != std::string::npos)
			target = target.substr(slash + 1);
		if (target.empty() || data.size() > 512)
			break;
		const size_t dir_end = url.find_last_of('/');
		url = url.substr(0, dir_end + 1) + UrlEncode(target);
		OrbisLog("[covers] %s is a link to %s", label.c_str(), target.c_str());
		status = http.Get(url, data);
	}
	if (status == 200 && !IsImage(data))
		status = -3; // not an image
	return status;
}
} // namespace

int FetchCoverUrl(HttpClient& http, std::string urls, const std::string& label, std::vector<uint8_t>& data)
{
	// several addresses, tab-separated (CoverUrlFor): the first that has an image; 404 only when none has
	int status = -1;
	size_t start = 0;
	while (start <= urls.size())
	{
		size_t end = urls.find('\t', start);
		if (end == std::string::npos)
			end = urls.size();
		const std::string url = urls.substr(start, end - start);
		start = end + 1;
		if (url.empty())
			continue;
		status = FetchOne(http, url, label, data);
		if (status != 404)
			return status; // an image, or a failure that isn't "not there" (offline...): no use asking further
	}
	return status;
}

bool SafeCoverFile(const std::string& f)
{
	// "<name>.png" in the covers folder: no folder part, so no way out of it
	return !f.empty() && f.size() < 250 && f.find('/') == std::string::npos && f.find("..") == std::string::npos &&
		   f.find('\\') == std::string::npos && f.find('\0') == std::string::npos && f[0] != '.' && f.size() > 4 &&
		   f.compare(f.size() - 4, 4, ".png") == 0;
}

std::vector<WantedCover> ParseWantedList(const std::string& text)
{
	std::vector<WantedCover> wanted;
	size_t start = 0;
	while (start < text.size())
	{
		const size_t end = text.find('\n', start);
		if (end == std::string::npos)
			break; // a last line without its end could be cut: skip it
		const std::string line = text.substr(start, end - start);
		start = end + 1;
		const size_t tab = line.find('\t');
		if (tab == std::string::npos)
			continue;
		WantedCover w{line.substr(0, tab), line.substr(tab + 1)};
		// the addresses (one or more, tab-separated): each one http(s)
		bool urls_ok = !w.url.empty();
		for (size_t s = 0; urls_ok && s <= w.url.size();)
		{
			size_t e = w.url.find('\t', s);
			if (e == std::string::npos)
				e = w.url.size();
			const std::string u = w.url.substr(s, e - s);
			urls_ok = u.compare(0, 8, "https://") == 0 || u.compare(0, 7, "http://") == 0;
			s = e + 1;
		}
		if (SafeCoverFile(w.file) && urls_ok)
			wanted.push_back(w);
	}
	return wanted;
}

int SaveCoverResult(const std::string& covers_dir, const WantedCover& w, int status, const std::vector<uint8_t>& data)
{
	const std::string path = covers_dir + "/" + w.file;
	const std::string base = path.substr(0, path.size() - 4);
	OrbisMkdirs(path.substr(0, path.find_last_of('/')));
	if (status == 200 && !data.empty())
	{
		if (WriteFileAtomicTo(path, data))
		{
			unlink((base + ".refetch").c_str()); // Square's request is done
			return 1;
		}
		OrbisLog("[covers] can't write %s", path.c_str());
		return -1;
	}
	if (status == 404)
	{
		// no new try for 30 days (Square on the shelf asks again); a cover already there stays
		unlink((base + ".refetch").c_str());
		if (FILE* f = fopen((base + ".missing").c_str(), "w"))
		{
			fprintf(f, "%s\n", w.url.c_str());
			fclose(f);
		}
		return 0;
	}
	return -1;
}
} // namespace fe
