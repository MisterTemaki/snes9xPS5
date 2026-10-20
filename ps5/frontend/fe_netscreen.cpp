// Snes9x PS5 frontend: the Network shares screen (Settings > Network shares).
//
// Lists the shares of shares.txt with their state, and a form to add one, change one, test it and remove it.
// The form writes the file through netfs_file_save_share (one line changes, the comments stay) and then asks
// netfs to load it again, so a saved share starts connecting at once. Passwords are shown as * everywhere.
//
// SPDX-License-Identifier: MIT

#include "fe_menu.h"

#include "fe_i18n.h"
#include "fe_osk.h"
#include "fe_text.h"
#include "fe_ui.h"

#include "OrbisPaths.h"
#include "ProsperoVideo.h"

#include "netfs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace fe
{
using namespace ui;

namespace
{
using netfs::Scheme;

// "SMB  192.168.1.50/games/snes": the mount point has no user and no password in it.
std::string ShortName(const netfs::ShareConfig& c)
{
	return std::string(c.scheme == Scheme::Smb ? "SMB  " : "NFS  ") + netfs_mount_point(c).substr(5);
}

const char* StateName(NetfsState s)
{
	switch (s)
	{
		case NetfsState::Idle:
		case NetfsState::Connecting: return T("Connecting...");
		case NetfsState::Ready: return T("Connected");
		case NetfsState::Failed: return T("Not connected");
		case NetfsState::Dead: return T("Connection lost");
	}
	return "";
}

// Cuts `text` at spaces into at most `max_lines` lines that fit `max_px`; the last one ends in "..." if more follows.
std::vector<std::string> Wrap(const std::string& text, int scale, int max_px, size_t max_lines)
{
	std::vector<std::string> lines;
	std::string line;
	size_t i = 0;
	while (i < text.size())
	{
		size_t e = text.find(' ', i);
		if (e == std::string::npos)
			e = text.size();
		const std::string word = text.substr(i, e - i);
		const std::string next = line.empty() ? word : line + " " + word;
		if (!line.empty() && TextWidth(next.c_str(), scale) > max_px)
		{
			if (lines.size() + 1 == max_lines)
			{
				lines.push_back(FitText(line + " " + text.substr(i), scale, max_px));
				return lines;
			}
			lines.push_back(line);
			line = word;
		}
		else
			line = next;
		i = e + 1;
	}
	if (!line.empty())
		lines.push_back(line);
	return lines;
}

void DrawNote(const std::string& text, uint32_t color)
{
	int y = H - 150;
	for (const std::string& l : Wrap(text, 3, W - 160, 2))
	{
		DrawText(80, y, l.c_str(), 3, color, false);
		y += 36;
	}
}

// A question with two answers over the same box the screen uses. Returns true for the first.
bool Ask(const char* title, const char* yes, const char* no)
{
	NavReader nav;
	int sel = 1; // the safe answer is where the cursor starts
	for (;;)
	{
		Header(title);
		DrawOptionBox(title, {{yes, ""}, {no, ""}}, sel, false);
		Footer((std::string(icon::Cross) + " " + T("Choose") + "     " + icon::Circle + " " + no).c_str());
		Present();
		const Nav n = nav.Read();
		if (n.up || n.down)
			sel = 1 - sel;
		if (n.back)
			return false;
		if (n.ok)
			return sel == 0;
	}
}

// ---- the form ----

enum class Item
{
	Type,
	Server,
	Share,
	Folder,
	User,
	Password,
	Domain,
	SmbVersion,
	Sign,
	Seal,
	NfsVersion,
	Uid,
	Gid,
	Test,
	Save,
	Remove,
	Cancel,
};

std::vector<Item> ItemsFor(Scheme scheme, bool existing)
{
	std::vector<Item> v = {Item::Type, Item::Server};
	if (scheme == Scheme::Smb)
		v.insert(v.end(), {Item::Share, Item::Folder, Item::User, Item::Password, Item::Domain, Item::SmbVersion, Item::Sign, Item::Seal});
	else
		v.insert(v.end(), {Item::Folder, Item::NfsVersion, Item::Uid, Item::Gid});
	v.insert(v.end(), {Item::Test, Item::Save});
	if (existing)
		v.push_back(Item::Remove);
	v.push_back(Item::Cancel);
	return v;
}

const char* const kSmbVersions[] = {"", "2", "3", "2.02", "2.10", "3.00", "3.02", "3.1.1"};

std::string OrDefault(const std::string& v, const char* when_empty)
{
	return v.empty() ? when_empty : v;
}

Row FormRow(Item it, const NetfsShareFields& f)
{
	const bool smb = f.scheme == Scheme::Smb;
	switch (it)
	{
		case Item::Type: return {T("Share type"), smb ? "SMB (Windows, Samba, NAS)" : "NFS"}; // names
		case Item::Server: return {T("Server"), OrDefault(f.server, T("IP address, like 192.168.1.50"))};
		case Item::Share: return {T("Share name"), OrDefault(f.share, T("required"))};
		case Item::Folder:
			return smb ? Row{T("Folder in the share"), OrDefault(f.folder, T("the whole share"))}
					   : Row{T("Export path"), OrDefault(f.folder, T("like /export/snes"))};
		case Item::User: return {T("User name"), OrDefault(f.user, T("Guest"))};
		case Item::Password: return {T("Password"), f.password.empty() ? T("None") : "********"};
		case Item::Domain: return {T("Domain"), OrDefault(f.domain, T("None"))};
		case Item::SmbVersion: return {T("SMB version"), OrDefault(f.vers, T("Auto"))};
		case Item::Sign: return {T("Signing"), T(f.sign ? "On" : "Off")};
		case Item::Seal: return {T("Encryption"), T(f.seal ? "On" : "Off")};
		case Item::NfsVersion: return {T("NFS version"), std::to_string(f.nfs_version)};
		case Item::Uid: return {T("User ID"), OrDefault(f.uid, T("Default"))};
		case Item::Gid: return {T("Group ID"), OrDefault(f.gid, T("Default"))};
		case Item::Test: return {T("Test connection"), ""};
		case Item::Save: return {T("Save"), ""};
		case Item::Remove: return {T("Remove this share"), ""};
		case Item::Cancel: return {T("Cancel"), ""};
	}
	return {};
}

// Changes a switch (dir -1/+1). Returns false when `it` is not a switch.
bool Adjust(Item it, NetfsShareFields& f, int dir)
{
	switch (it)
	{
		case Item::Type: f.scheme = f.scheme == Scheme::Smb ? Scheme::Nfs : Scheme::Smb; return true;
		case Item::Sign: f.sign = !f.sign; return true;
		case Item::Seal: f.seal = !f.seal; return true;
		case Item::NfsVersion: f.nfs_version = f.nfs_version == 3 ? 4 : 3; return true;
		case Item::SmbVersion:
		{
			const int n = int(sizeof(kSmbVersions) / sizeof(kSmbVersions[0]));
			int i = 0;
			while (i < n && f.vers != kSmbVersions[i])
				i++;
			f.vers = kSmbVersions[(i + dir + n) % n];
			return true;
		}
		default: return false;
	}
}

// A text field: the string, the keyboard's title, whether it is hidden, the longest it may be. nullptr: not a text field.
std::string* TextField(Item it, NetfsShareFields& f, const char*& title, bool& secret, size_t& max_len)
{
	secret = false;
	max_len = 128;
	switch (it)
	{
		case Item::Server: title = T("Server: an IP address like 192.168.1.50"); max_len = 64; return &f.server;
		case Item::Share: title = T("Share name"); max_len = 80; return &f.share;
		case Item::Folder:
			title = f.scheme == Scheme::Smb ? T("Folder in the share (empty: the whole share)") : T("Export path, like /export/snes");
			max_len = 200;
			return &f.folder;
		case Item::User: title = T("User name (empty: guest)"); max_len = 64; return &f.user;
		case Item::Password: title = T("Password"); secret = true; return &f.password;
		case Item::Domain: title = T("Domain (usually empty)"); max_len = 64; return &f.domain;
		case Item::Uid: title = T("User ID (a number, empty: default)"); max_len = 9; return &f.uid;
		case Item::Gid: title = T("Group ID (a number, empty: default)"); max_len = 9; return &f.gid;
		default: return nullptr;
	}
}

// The form for a share in the file (`existing`) or a new one. Returns true when the file was changed (and netfs
// loaded it again).
bool RunForm(const NetfsFileShare* existing)
{
	NetfsShareFields f;
	if (existing)
		f = netfs_fields_from_config(existing->cfg);
	const std::string subtitle = existing ? ShortName(existing->cfg) : std::string(T("Add a share"));

	NavReader nav;
	int sel = 1; // the first thing to fill in
	bool dirty = false;
	std::string status;
	uint32_t status_color = kDim;
	std::unique_ptr<NetfsConnectionTest> test;
	netfs::ShareConfig testing;

	auto build = [&](netfs::ShareConfig& cfg) {
		std::string err;
		if (netfs_config_from_fields(f, cfg, err))
			return true;
		status = err;
		status_color = kBad;
		return false;
	};

	for (;;)
	{
		const std::vector<Item> items = ItemsFor(f.scheme, existing != nullptr);
		sel = std::min(sel, int(items.size()) - 1);
		if (test && test->Poll())
		{
			const NetfsTestResult& r = test->Result();
			status = r.text;
			status_color = r.ok ? kGood : kBad;
			OrbisLog("[net] test of %s: %s", netfs_redact(testing).c_str(), r.text.c_str());
			test.reset();
		}

		std::vector<Row> rows;
		for (Item it : items)
			rows.push_back(FormRow(it, f));
		Header(subtitle.c_str());
		DrawOptionBox(T(existing ? "Edit share" : "Add a share"), rows, sel, false);
		if (test)
		{
			const int dots = int(std::fmod(Now() * 3.0, 4.0));
			DrawNote(std::string(T("Testing the connection")) + std::string(size_t(dots), '.'), kDim);
		}
		else if (!status.empty())
			DrawNote(status, status_color);
		else if (existing == nullptr && sel == 1)
			DrawNote(T("Use the server's IP address: names often do not resolve on the console."), kDim);
		Footer(test ? (std::string(icon::Circle) + " " + T("Stop the test")).c_str()
					: (std::string(icon::Cross) + " " + T("Edit") + "     " + icon::DpadLeftRight + " " + T("Change") + "     " + icon::Circle + " " +
					   T("Back"))
						  .c_str());
		Present();

		const Nav n = nav.Read();
		if (test)
		{
			if (n.back)
			{
				test.reset();
				status = T("Test stopped.");
				status_color = kDim;
			}
			continue;
		}
		const int count = int(items.size());
		if (n.up)
			sel = (sel + count - 1) % count;
		if (n.down)
			sel = (sel + 1) % count;
		if (n.back || n.options)
		{
			if (!dirty || Ask(T("Discard your changes?"), T("Discard"), T("Keep editing")))
				return false;
			nav = NavReader();
			continue;
		}
		const Item it = items[size_t(sel)];
		if ((n.left || n.right) && Adjust(it, f, n.left ? -1 : 1))
		{
			dirty = true;
			status.clear();
			continue;
		}
		if (!n.ok)
			continue;
		if (Adjust(it, f, 1))
		{
			dirty = true;
			status.clear();
			continue;
		}
		const char* title = "";
		bool secret = false;
		size_t max_len = 128;
		if (std::string* field = TextField(it, f, title, secret, max_len))
		{
			if (EditText(title, *field, secret, max_len))
			{
				dirty = true;
				status.clear();
			}
			nav = NavReader();
			continue;
		}
		netfs::ShareConfig cfg;
		switch (it)
		{
			case Item::Test:
				if (build(cfg))
				{
					testing = cfg;
					test = std::make_unique<NetfsConnectionTest>(cfg);
					status.clear();
				}
				break;
			case Item::Save:
			{
				if (!build(cfg))
					break;
				const std::string err = netfs_file_save_share(existing ? existing->line : 0, existing ? existing->mount_point : std::string(), cfg);
				if (!err.empty())
				{
					status = err;
					status_color = kBad;
					break;
				}
				netfs_reload();
				return true;
			}
			case Item::Remove:
			{
				if (!Ask(T("Remove this share?"), T("Remove"), T("Keep it")))
				{
					nav = NavReader();
					break;
				}
				const std::string err = netfs_file_remove_share(existing->line, existing->mount_point);
				if (!err.empty())
				{
					status = err;
					status_color = kBad;
					nav = NavReader();
					break;
				}
				netfs_reload();
				return true;
			}
			case Item::Cancel:
				if (!dirty || Ask(T("Discard your changes?"), T("Discard"), T("Keep editing")))
					return false;
				nav = NavReader();
				break;
			default: break;
		}
	}
}
} // namespace

void NetworkMenu()
{
	NavReader nav;
	NetfsFileView view = netfs_file_shares();
	int sel = 0;
	std::string note;
	for (;;)
	{
		const std::vector<NetfsShareInfo> live = netfs_shares();
		std::vector<Row> rows;
		for (const NetfsFileShare& s : view.shares)
		{
			std::string state = T("Not used");
			for (const NetfsShareInfo& i : live)
				if (s.used && i.mount_point == s.mount_point)
					state = StateName(i.state);
			rows.push_back({ShortName(s.cfg), state, true, true});
		}
		const int add_row = int(rows.size());
		rows.push_back({T("Add a share"), ""});
		rows.push_back({T("Connect again"), ""});
		rows.push_back({T("Back"), ""});
		sel = std::min(sel, int(rows.size()) - 1);

		Header(T(view.error.empty() ? "Games on your network (SMB or NFS)" : "Network shares"));
		DrawOptionBox(T("Network shares"), rows, sel, false);
		if (!view.error.empty())
			DrawNote(view.error, kBad);
		else if (sel < add_row)
		{
			const NetfsFileShare& s = view.shares[size_t(sel)];
			std::string why;
			for (const NetfsShareInfo& i : live)
				if (s.used && i.mount_point == s.mount_point)
					why = i.error;
			if (!s.used)
				DrawNote(s.note, kDim);
			else if (!why.empty())
				DrawNote(why, kBad);
			else if (!note.empty())
				DrawNote(note, kDim);
		}
		else if (!note.empty())
			DrawNote(note, kDim);
		else if (view.shares.empty())
			DrawNote(T("No shares yet. Add one: the games on it show up in the list, saves stay on the console."), kDim);
		else if (view.invalid > 0)
		{
			char text[256];
			snprintf(text, sizeof(text),
				T(view.invalid == 1 ? "%d line of shares.txt is not a valid address and is skipped."
									: "%d lines of shares.txt are not valid addresses and are skipped."),
				int(view.invalid));
			DrawNote(text, kDim);
		}
		Footer((std::string(icon::Cross) + " " + T("Open") + "     " + icon::Circle + " " + T("Back")).c_str());
		Present();

		const Nav n = nav.Read();
		const int count = int(rows.size());
		if (n.up)
			sel = (sel + count - 1) % count;
		if (n.down)
			sel = (sel + 1) % count;
		if (n.back || n.options || (n.ok && sel == count - 1))
			return;
		if (!n.ok || !view.error.empty())
			continue;
		note.clear();
		if (sel < add_row)
		{
			RunForm(&view.shares[size_t(sel)]);
			view = netfs_file_shares();
			nav = NavReader();
		}
		else if (sel == add_row)
		{
			int used = 0;
			for (const NetfsFileShare& s : view.shares)
				used += s.used;
			if (used >= kNetfsMaxShares)
			{
				char text[128];
				snprintf(text, sizeof(text), T("Up to %d shares are used. Remove one first."), int(kNetfsMaxShares));
				note = text;
			}
			else
			{
				if (RunForm(nullptr))
					sel = int(netfs_file_shares().shares.size()) - 1;
				view = netfs_file_shares();
				nav = NavReader();
			}
		}
		else
		{
			netfs_reload();
			note = T("Connecting to every share again.");
		}
	}
}
} // namespace fe
