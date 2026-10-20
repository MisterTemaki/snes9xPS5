// Our other copies in /data/homebrew (ProsperoMigrate.h): found, never touched.
//
// SPDX-License-Identifier: MIT

#include "ProsperoMigrate.h"

#include "OrbisPaths.h"
#include "ProsperoMigrateConfig.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace migrate
{
namespace
{
std::string HomebrewRoot()
{
	const char* v = getenv(MIG_ENV_PREFIX "PS5_HOMEBREW");
	return v && *v ? v : "/data/homebrew";
}

std::string ReadSmall(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return "";
	std::string s(1 << 16, '\0');
	const size_t n = fread(&s[0], 1, s.size(), f);
	fclose(f);
	s.resize(n);
	return s;
}

// The value of "key": "..." in a small JSON text ("" when it isn't there).
std::string JsonString(const std::string& json, const char* key)
{
	const std::string k = std::string("\"") + key + "\"";
	size_t p = json.find(k);
	if (p == std::string::npos)
		return "";
	p = json.find(':', p + k.size());
	if (p == std::string::npos)
		return "";
	p = json.find('"', p + 1);
	if (p == std::string::npos)
		return "";
	const size_t e = json.find('"', p + 1);
	return e == std::string::npos ? "" : json.substr(p + 1, e - p - 1);
}
} // namespace

std::vector<OtherInstall> FindOtherInstalls()
{
	std::vector<OtherInstall> found;
	const std::string root = HomebrewRoot();
	DIR* d = opendir(root.c_str());
	if (!d)
		return found;
	std::vector<std::string> names;
	while (const dirent* e = readdir(d))
		if (e->d_name[0] != '.')
			names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());

	const std::string tail = "_00-" MIG_CONTENT_LABEL;
	for (const std::string& name : names)
	{
		if (name == MIG_NEW_TITLE)
			continue;
		const std::string dir = root + "/" + name;
		const std::string param = ReadSmall(dir + "/sce_sys/param.json");
		if (param.empty())
			continue;
		const std::string content_id = JsonString(param, "contentId");
		const bool ours = content_id.compare(0, 7, "UP9000-") == 0 && content_id.size() > tail.size() &&
						  content_id.compare(content_id.size() - tail.size(), tail.size(), tail) == 0;
		OrbisLog("[migrate] %s: %s, left as it is", dir.c_str(),
			ours ? "another copy of Snes9x PS5 (a second icon)" : "another app's folder");
		if (ours)
			found.push_back({name, JsonString(param, "titleId")});
	}
	return found;
}
} // namespace migrate
