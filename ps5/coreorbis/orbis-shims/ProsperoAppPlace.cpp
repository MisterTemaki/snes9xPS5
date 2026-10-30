// Snes9x PS5 2.10 (from Stella PS5 1.0, itself from Media Player Classic PS5 1.5's ShadowMount+ fix and 1.5.1): where
// the app the console runs really is (ProsperoAppPlace.h).
//
// Built into the payloads only (the installer and the helper, which installs the app's updates): it reads the mount
// table with statfs(2) / getfsstat(2), which the SDK has only in libkernel_sys, a library a title can't load (Media
// Player Classic PS5 1.5 imported it and crashed at launch). The app itself never writes the app folder, so it needs
// none of this (tools/check_imports.py checks the eboot has no such import).
//
// SPDX-License-Identifier: MIT
#include "ProsperoAppPlace.h"
#include "ProsperoPlaceText.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__PROSPERO__) || defined(__FreeBSD__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// statfs(2)'s f_mntonname / f_mntfromname hold MNAMELEN (88) bytes with the NUL: a longer name comes back cut
constexpr size_t kMountName = 88;

struct MountInfo
{
	bool ok = false;
	std::string on, type, from;
	bool ro = false;
	bool from_cut = false; // f_mntfromname was full: the source's name may be cut short
};

bool Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

bool Inside(const std::string& path, const std::string& on)
{
	return on == "/" || path == on || (path.size() > on.size() && path.compare(0, on.size(), on) == 0 && path[on.size()] == '/');
}

// the longest mount point a path is in; of the mounts on one point the last one (the top)
[[maybe_unused]] MountInfo Lookup(const std::vector<MountInfo>& table, const std::string& path)
{
	MountInfo best;
	for (const MountInfo& m : table)
		if (Inside(path, m.on) && (!best.ok || m.on.size() >= best.on.size()))
			best = m;
	return best;
}

#if defined(__PROSPERO__) || defined(__FreeBSD__)
MountInfo FromStatfs(const struct statfs& sf)
{
	MountInfo m;
	m.ok = true;
	m.on.assign(sf.f_mntonname, strnlen(sf.f_mntonname, sizeof(sf.f_mntonname)));
	m.type.assign(sf.f_fstypename, strnlen(sf.f_fstypename, sizeof(sf.f_fstypename)));
	m.from.assign(sf.f_mntfromname, strnlen(sf.f_mntfromname, sizeof(sf.f_mntfromname)));
	m.from_cut = m.from.size() + 1 >= kMountName;
	m.ro = (sf.f_flags & MNT_RDONLY) != 0;
	return m;
}

bool StatfsOf(const std::string& path, MountInfo& m)
{
	struct statfs sf;
	memset(&sf, 0, sizeof(sf));
	if (statfs(path.c_str(), &sf) != 0)
		return false;
	m = FromStatfs(sf);
	return true;
}

// every mount, in the order they were made (getfsstat(2), as getmntinfo(3) does)
bool MountTable(std::vector<MountInfo>& out)
{
	for (int tries = 0; tries < 3; tries++)
	{
		const int n = getfsstat(nullptr, 0, MNT_NOWAIT);
		if (n < 0)
			return false;
		std::vector<struct statfs> v(size_t(n) + 8);
		const int got = getfsstat(v.data(), long(v.size() * sizeof(struct statfs)), MNT_NOWAIT);
		if (got < 0)
			return false;
		if (size_t(got) >= v.size())
			continue; // more mounts meanwhile
		out.clear();
		for (int i = 0; i < got; i++)
			out.push_back(FromStatfs(v[size_t(i)]));
		return true;
	}
	return false;
}
#else
// The host tests: SNES9X_HOST_MOUNTS is the table ("<on>|<type>|<from>|ro or rw;...", in mount order, names cut at
// 87 bytes as the console's), "/" under it all; SNES9X_HOST_NO_STATFS: statfs(2) fails; SNES9X_HOST_NO_GETFSSTAT:
// getfsstat(2) fails
void HostTable(std::vector<MountInfo>& out)
{
	out.clear();
	MountInfo root;
	root.ok = true;
	root.on = "/";
	root.type = "host";
	out.push_back(root);
	const char* env = getenv("SNES9X_HOST_MOUNTS");
	if (!env)
		return;
	const std::string table = env;
	size_t at = 0;
	while (at < table.size())
	{
		size_t end = table.find(';', at);
		if (end == std::string::npos)
			end = table.size();
		const std::string row = table.substr(at, end - at);
		at = end + 1;
		std::vector<std::string> f;
		size_t p = 0;
		for (;;)
		{
			const size_t bar = row.find('|', p);
			f.push_back(row.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
			if (bar == std::string::npos)
				break;
			p = bar + 1;
		}
		if (f.size() < 4 || f[0].empty())
			continue;
		MountInfo m;
		m.ok = true;
		m.on = f[0].substr(0, kMountName - 1);
		m.type = f[1];
		m.from = f[2].substr(0, kMountName - 1);
		m.from_cut = m.from.size() + 1 >= kMountName;
		m.ro = f[3] == "ro";
		out.push_back(m);
	}
}

bool MountTable(std::vector<MountInfo>& out)
{
	if (getenv("SNES9X_HOST_NO_GETFSSTAT"))
		return false;
	HostTable(out);
	return true;
}

bool StatfsOf(const std::string& path, MountInfo& m)
{
	if (getenv("SNES9X_HOST_NO_STATFS") || !Exists(path))
		return false;
	std::vector<MountInfo> table;
	HostTable(table);
	m = Lookup(table, path);
	return m.ok;
}
#endif

// the mount a path is on: statfs(2), else the mount table
MountInfo MountOf(const std::string& path)
{
	MountInfo m;
	if (StatfsOf(path, m))
		return m;
	std::vector<MountInfo> table;
	if (Exists(path) && MountTable(table))
		return Lookup(table, path);
	return MountInfo();
}

// a file can be made (and removed) there
bool CanWrite(const std::string& dir)
{
	const std::string probe = dir + "/.snes9xps5-write-test";
	const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	const bool wrote = write(fd, "x", 1) == 1;
	close(fd);
	unlink(probe.c_str());
	return wrote;
}

// the folder's sce_sys/param.json says it is this app ("titleId": "<TITLE_ID>")
bool NamesTitle(const std::string& dir, const std::string& title_id)
{
	FILE* f = fopen((dir + "/sce_sys/param.json").c_str(), "rb");
	if (!f)
		return false;
	std::string s(65536, '\0');
	s.resize(fread(&s[0], 1, s.size(), f));
	fclose(f);
	const size_t k = s.find("\"titleId\"");
	if (k == std::string::npos)
		return false;
	const size_t colon = s.find(':', k);
	const size_t a = colon == std::string::npos ? colon : s.find('"', colon + 1);
	const size_t b = a == std::string::npos ? a : s.find('"', a + 1);
	return b != std::string::npos && s.compare(a + 1, b - a - 1, title_id) == 0;
}

// the source's eboot.bin is the one the mount shows (nullfs passes the inode number through)
bool SameEboot(const std::string& source, const std::string& mount)
{
	struct stat a, b;
	if (stat((mount + "/eboot.bin").c_str(), &b) != 0)
		return true; // nothing to compare with
	return stat((source + "/eboot.bin").c_str(), &a) == 0 && a.st_ino == b.st_ino && a.st_size == b.st_size;
}

// ShadowMount+'s images (.ffpkg ufs, .exfat, .ffpfs pfs) are mounted under /mnt/shadowmnt. Anything else is judged by
// its read-only flag and whether a file can be written (extended storage may be on an LVD device and writable)
bool InImages(const std::string& path)
{
	const char* env = getenv("SNES9X_SHADOWMNT"); // the host tests' stand-in
	return Inside(path, env && *env ? env : "/mnt/shadowmnt");
}

bool IsImage(const MountInfo& u, const std::string& source)
{
	return InImages(u.on) || InImages(source);
}

std::string Line(const char* fmt, const std::string& a, const std::string& b = "", const std::string& c = "")
{
	char line[1024];
	snprintf(line, sizeof(line), fmt, a.c_str(), b.c_str(), c.c_str());
	return line;
}

// a reason for the user (ProsperoPlaceText.h): the template with the folder in it
std::string Reason(const char* fmt, const std::string& folder)
{
	char text[1024];
	snprintf(text, sizeof(text), fmt, folder.c_str());
	return text;
}

void SetImage(AppPlace& a)
{
	a.writable = false;
	a.image = true;
	a.reason = Reason(placetext::kImage, a.source);
}

// The folder can't be told for sure, and the evidence says writing /data/homebrew would make a second copy (ShadowMount+
// names a folder that isn't there now): nothing is written.
void Unsure(AppPlace& a, const std::string& reason, const std::string& log)
{
	a.writable = false;
	a.unsure = true;
	a.reason = reason;
	a.log += log;
}

// 2.10 review: something is mounted on /system_ex/app/<TITLE_ID> that can't be read as ShadowMount+'s folder mount (no
// mount table, not nullfs, a source that can't be identified). 2.9 wrote default_dir whatever was mounted; nothing says
// that is wrong here, so it is done again (never worse than 2.9), and the log says why.
void FallBack(AppPlace& a, const std::string& default_dir, const std::string& log)
{
	a.writable = true;
	a.unsure = false;
	a.fallback = true;
	a.via_link = false;
	a.mount.clear(); // the version compared and the folder written are default_dir's, as in 2.9
	a.source = a.dir = default_dir;
	a.reason.clear();
	a.log += log + "; files go to " + default_dir + ", as 2.9 wrote them";
}

// a.source found: what it is on, and where its files can be written (candidates: through the mount first)
void Finish(AppPlace& a, const std::string& title_id, bool mount_ro, const std::vector<std::string>& candidates)
{
	// the file system the source is on (through nullfs layers, if any)
	MountInfo u = MountOf(a.source);
	std::string at = a.source;
	for (int depth = 0; u.ok && u.type == "nullfs" && u.on == at && !u.from.empty() && !u.from_cut && depth < 4; depth++)
	{
		at = u.from;
		u = MountOf(at);
	}
	a.fs = u.ok ? u.type : "";
	a.image = IsImage(u, a.source);
	if (u.ok)
		a.log += Line(" (on %s, %s, from %s", u.on, a.fs, u.from) + (mount_ro || u.ro ? ", read-only)" : ")");
	a.dir = a.source;
	if (a.image)
	{
		SetImage(a);
		a.log += ": an image, not written";
		return;
	}
	if (u.ok && u.ro)
	{
		a.writable = false;
		a.reason = Reason(placetext::kReadOnly, a.source);
		a.log += ": read-only, not written";
		return;
	}
	// 2.10 review: a read-only mount of a folder whose own file system takes files (a nullfs mounted ro over
	// /data/homebrew/<TITLE_ID>): not through the mount, but the folder itself (2.9 wrote it too)
	std::vector<std::string> usable;
	for (const std::string& d : candidates)
		if (!(mount_ro && d == a.mount))
			usable.push_back(d);
	if (mount_ro)
		a.log += usable.empty() ? ": the mount is read-only" : ": the mount is read-only, the folder itself is written";
	if (usable.empty())
	{
		a.writable = false;
		a.reason = Reason(placetext::kReadOnly, a.source);
		a.log += ", not written";
		return;
	}
	std::string tried;
	for (const std::string& d : usable)
	{
		tried += (tried.empty() ? "" : " nor ") + d;
		if (NamesTitle(d, title_id) && CanWrite(d))
		{
			a.dir = d;
			a.log += ": files go through " + d;
			return;
		}
	}
	a.writable = false;
	a.reason = Reason(placetext::kNoWrite, a.source);
	a.log += ": neither " + tried + " takes a file";
}

// No mount on /system_ex/app/<TITLE_ID> (yet: a payload autoloaded before ShadowMount+ has mounted the app): the
// folder ShadowMount+ last mounted there, from its link /user/app/<TITLE_ID>/mount.lnk
// ShadowMount+'s link for the title: its folder (/user/app/<TITLE_ID>/mount.lnk), "" when there is none
std::string ReadLink(const std::string& title_id, std::string& links, std::string& lnk)
{
	const char* ua = getenv("SNES9X_USER_APP");
	links = std::string(ua && *ua ? ua : "/user/app") + "/" + title_id;
	lnk = links + "/mount.lnk";
	std::string target;
	if (FILE* f = fopen(lnk.c_str(), "r"))
	{
		char buf[1024] = "";
		if (fgets(buf, sizeof(buf), f))
		{
			buf[strcspn(buf, "\r\n")] = 0;
			target = buf;
		}
		fclose(f);
	}
	while (target.size() > 1 && target.back() == '/')
		target.pop_back();
	return target;
}

void NoMount(AppPlace& a, const std::string& title_id, const std::string& default_dir, const std::string& mount)
{
	std::string links, lnk;
	const std::string target = ReadLink(title_id, links, lnk);
	a.log = mount + " isn't a mount";
	if (!target.empty() && target[0] != '/')
	{
		a.log += " and ShadowMount+'s link holds \"" + target + "\", not a folder's path: ignored; files go to " + default_dir;
		return;
	}
	// an image's folder (ShadowMount+ mounts the image under /mnt/shadowmnt and writes mount_img.lnk beside the link):
	// never written, and before ShadowMount+ has mounted it, nothing to say
	if (!target.empty() && (InImages(target) || Exists(links + "/mount_img.lnk")))
	{
		a.source = a.dir = target;
		a.via_link = true;
		a.log += "; ShadowMount+ runs the app from an image's folder, " + target + " (" + lnk + ")";
		if (!Exists(target))
		{
			SetImage(a);
			a.image_pending = true;
			a.log += ", not mounted yet: not written";
			return;
		}
		Finish(a, title_id, false, {target});
		if (!a.image)
		{
			SetImage(a);
			a.dir = target;
			a.log += "; an image, not written";
		}
		return;
	}
	if (target.empty() || target == default_dir)
	{
		a.log += target.empty() ? " and ShadowMount+ has no link for the app: files go to " + default_dir
								: " (ShadowMount+'s link: " + target + "): files go to " + default_dir;
		return;
	}
	if (!Exists(target))
	{
		a.source = a.dir = target;
		Unsure(a, Reason(placetext::kGone, target),
			" and ShadowMount+'s link names " + target + ", which isn't there: not written (" + default_dir +
				" would be a second copy)");
		return;
	}
	if (!NamesTitle(target, title_id))
	{
		a.log += " and ShadowMount+'s link names " + target + ", which doesn't hold " + title_id + ": files go to " +
				 default_dir;
		return;
	}
	a.source = target;
	a.via_link = true;
	a.log += "; ShadowMount+ runs the app from " + target + " (" + lnk + ")";
	Finish(a, title_id, false, {target});
}

} // namespace

AppPlace FindAppPlace(const std::string& title_id, const std::string& default_dir)
{
	AppPlace a;
	a.source = a.dir = default_dir;
	const char* sx = getenv("SNES9X_SYSTEM_EX");
	const std::string mount = std::string(sx && *sx ? sx : "/system_ex") + "/app/" + title_id;
	std::vector<MountInfo> table;
	const bool have_table = MountTable(table);
	MountInfo top;
	if (!StatfsOf(mount, top) && Exists(mount))
	{
		// statfs(2) failed: the mount table says what is there
		if (!have_table)
		{
			a.log = mount + ": ";
			FallBack(a, default_dir, "neither statfs nor getfsstat answers");
			return a;
		}
		top = Lookup(table, mount);
	}
	if (!top.ok || top.on != mount)
	{
		NoMount(a, title_id, default_dir, mount);
		return a;
	}
	a.mount = mount;
	// ShadowMount+ mounts the app's folder with nullfs. Something on top of it (a backport's unionfs) isn't
	// written into: the nullfs layer beneath gives the folder, written directly
	const MountInfo* layer = nullptr;
	bool through_mount = false;
	if (top.type == "nullfs")
	{
		layer = &top;
		through_mount = true;
	}
	else
		for (auto it = table.rbegin(); it != table.rend(); ++it)
			if (it->on == mount && it->type == "nullfs")
			{
				layer = &*it;
				break;
			}
	a.log = mount + " is a " + top.type + " mount";
	if (!layer)
	{
		FallBack(a, default_dir, ", no nullfs mount of a folder under it");
		return a;
	}
	if (!through_mount)
		a.log += " over a nullfs mount";
	// the folder: named by the mount (in full) and the app's own, with the eboot the console sees
	const std::string from = layer->from;
	const bool known = !layer->from_cut && !from.empty() && NamesTitle(from, title_id) &&
					   (!through_mount || SameEboot(from, mount));
	if (known)
	{
		a.source = from;
		a.log += " of " + from;
		std::vector<std::string> cand;
		if (through_mount)
			cand.push_back(mount);
		cand.push_back(from);
		Finish(a, title_id, through_mount && top.ro, cand);
		return a;
	}
	const std::string why = layer->from_cut ? " (its name cut short by the system: \"" + from + "\")"
											: " of " + from + ", which isn't the folder it shows";
	if (through_mount && NamesTitle(mount, title_id))
	{
		// only through the mount, which is the app whatever its folder's name
		a.source = mount;
		a.log += why + "; written only through the mount";
		Finish(a, title_id, top.ro, {mount});
		return a;
	}
	FallBack(a, default_dir, why);
	return a;
}
