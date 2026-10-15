// The app's earlier title IDs (ProsperoMigrate.h): found, never touched.
//
// SPDX-License-Identifier: MIT

#include "ProsperoMigrate.h"

#include "OrbisPaths.h"
#include "ProsperoMigrateConfig.h"

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

std::vector<std::string> OldIds()
{
	std::vector<std::string> out;
	const std::string s = MIG_OLD_TITLES;
	size_t a = 0;
	while (a <= s.size())
	{
		size_t b = s.find(',', a);
		if (b == std::string::npos)
			b = s.size();
		std::string id = s.substr(a, b - a);
		while (!id.empty() && id[0] == ' ')
			id.erase(0, 1);
		if (id.size() == 9 && id != MIG_NEW_TITLE)
			out.push_back(id);
		a = b + 1;
	}
	return out;
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

std::string ContentIdFor(const char* title_id)
{
	return std::string("UP9000-") + title_id + "_00-" MIG_CONTENT_LABEL;
}

std::string FindOldInstalls()
{
	std::string found;
	for (const std::string& id : OldIds())
	{
		const std::string dir = HomebrewRoot() + "/" + id;
		if (!OrbisIsDir(dir))
			continue;
		const std::string param = ReadSmall(dir + "/sce_sys/param.json");
		const bool ours = !param.empty() && JsonString(param, "titleId") == id &&
						  JsonString(param, "contentId") == ContentIdFor(id.c_str());
		OrbisLog("[migrate] %s: %s, left as it is", dir.c_str(),
			ours ? "our earlier version (the old icon)" : "another app's folder");
		if (ours)
			found += (found.empty() ? "" : ", ") + id;
	}
	return found;
}
} // namespace migrate
