// Snes9x PS5 netfs: parsing of shares.txt, mount points, redaction (netfs_share.h).
//
// SPDX-License-Identifier: MIT
#include "netfs_share.h"

#include <cctype>
#include <set>

using netfs::ErrKind;
using netfs::Scheme;
using netfs::ShareConfig;
using netfs::Status;

namespace
{
int HexVal(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

enum class Dec
{
	Ok,
	BadEscape,
	BadChar,
};

// Percent-decodes `in`. Control characters (also written as %00..%1F or %7F) are refused: no name has them.
Dec PctDecode(const std::string& in, std::string& out)
{
	out.clear();
	for (size_t i = 0; i < in.size(); i++)
	{
		unsigned char c = (unsigned char)in[i];
		if (c == '%')
		{
			if (i + 2 >= in.size())
				return Dec::BadEscape;
			int hi = HexVal(in[i + 1]), lo = HexVal(in[i + 2]);
			if (hi < 0 || lo < 0)
				return Dec::BadEscape;
			c = (unsigned char)(hi * 16 + lo);
			i += 2;
		}
		if (c < 0x20 || c == 0x7f)
			return Dec::BadChar;
		out.push_back((char)c);
	}
	return Dec::Ok;
}

// For text shown to the player: the characters that would make an address ambiguous become %XX.
std::string PctEncodeDisplay(const std::string& s)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if (c <= 0x20 || c == 0x7f || c == '%' || c == '&' || c == '=' || c == '@' || c == ';' || c == '/' || c == '?' ||
			c == '#' || c == ':')
		{
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
		else
			out.push_back((char)c);
	}
	return out;
}

// Everything but letters and digits as %XX: the form a password could take in an address.
std::string PctEncodeAll(const std::string& s)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if (isalnum(c))
			out.push_back((char)c);
		else
		{
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
	}
	return out;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

bool Fail(std::string& err, const char* msg)
{
	err = msg;
	return false;
}

bool DecodeField(const std::string& in, std::string& out, const char* what, std::string& err)
{
	Dec d = PctDecode(in, out);
	if (d == Dec::Ok)
		return true;
	err = std::string(d == Dec::BadEscape ? "bad % escape in the " : "unusable character in the ") + what;
	return false;
}

// Digits only, within [lo, hi].
bool ParseInt(const std::string& s, long lo, long hi, long& out)
{
	if (s.empty() || s.size() > 9)
		return false;
	long v = 0;
	for (char c : s)
	{
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + (c - '0');
	}
	if (v < lo || v > hi)
		return false;
	out = v;
	return true;
}

bool ParseHostPort(const std::string& hp, ShareConfig& cfg, std::string& err)
{
	std::string host = hp, port;
	bool has_port = false;
	if (!host.empty() && host[0] == '[')
	{
		size_t e = host.find(']');
		if (e == std::string::npos)
			return Fail(err, "the host name has a [ without a ]");
		std::string rest = host.substr(e + 1);
		host.resize(e + 1);
		if (!rest.empty())
		{
			if (rest[0] != ':')
				return Fail(err, "bad host name");
			has_port = true;
			port = rest.substr(1);
		}
		for (size_t i = 1; i + 1 < host.size(); i++)
			if (HexVal(host[i]) < 0 && host[i] != ':' && host[i] != '.')
				return Fail(err, "bad host name");
	}
	else
	{
		size_t c = host.rfind(':');
		if (c != std::string::npos)
		{
			has_port = true;
			port = host.substr(c + 1);
			host.resize(c);
		}
		for (char ch : host)
			if (!isalnum((unsigned char)ch) && ch != '.' && ch != '-' && ch != '_')
				return Fail(err, "bad host name");
	}
	if (host.empty() || host == "[]")
		return Fail(err, "the host name is missing");
	if (has_port)
	{
		long p = 0;
		if (!ParseInt(port, 1, 65535, p))
			return Fail(err, "the port must be a number from 1 to 65535");
		cfg.port = (int)p;
	}
	cfg.host = Lower(host);
	return true;
}

// Splits a path on '/', drops empty segments, decodes each. "." and ".." are refused, as is a '/' or (SMB) a
// '\' that was hidden in a %XX: a segment is one name.
bool ParseSegments(const std::string& path, bool smb, std::vector<std::string>& out, std::string& err)
{
	size_t i = 0;
	while (i <= path.size())
	{
		size_t e = path.find('/', i);
		if (e == std::string::npos)
			e = path.size();
		if (e > i)
		{
			std::string seg;
			if (!DecodeField(path.substr(i, e - i), seg, "folder name", err))
				return false;
			if (seg == "." || seg == "..")
				return Fail(err, "a folder name of . or .. is not allowed");
			if (seg.find('/') != std::string::npos || (smb && seg.find('\\') != std::string::npos))
				return Fail(err, "a folder name has a / or \\ in it");
			out.push_back(seg);
		}
		i = e + 1;
	}
	return true;
}

bool ParseBool(bool has_value, const std::string& v, bool& out)
{
	if (!has_value)
	{
		out = true;
		return true;
	}
	std::string l = Lower(v);
	if (l == "1" || l == "true" || l == "yes" || l == "on")
		out = true;
	else if (l == "0" || l == "false" || l == "no" || l == "off")
		out = false;
	else
		return false;
	return true;
}

// `query` is the text after '?': split on '&', each option on its first '=', and only then decoded, so an
// encoded delimiter never splits an option.
bool ParseQuery(const std::string& query, ShareConfig& cfg, std::string& err)
{
	std::set<std::string> seen;
	size_t i = 0;
	while (i <= query.size())
	{
		size_t e = query.find('&', i);
		if (e == std::string::npos)
			e = query.size();
		std::string item = query.substr(i, e - i);
		i = e + 1;
		if (item.empty())
			continue;
		size_t eq = item.find('=');
		bool has_value = eq != std::string::npos;
		std::string name, value;
		if (!DecodeField(item.substr(0, eq), name, "option name", err) ||
			!DecodeField(has_value ? item.substr(eq + 1) : std::string(), value, "option value", err))
			return false;
		name = Lower(name);
		if (!seen.insert(name).second)
			return Fail(err, "an option is given twice");

		bool smb = cfg.scheme == Scheme::Smb;
		long n = 0;
		if (smb && name == "password")
			cfg.password = value;
		else if (smb && name == "vers")
		{
			static const char* ok[] = {"2", "3", "2.02", "2.10", "3.00", "3.02", "3.1.1"};
			bool found = false;
			for (const char* o : ok)
				found = found || value == o;
			if (!found)
				return Fail(err, "vers must be 2, 3, 2.02, 2.10, 3.00, 3.02 or 3.1.1");
			cfg.vers = value;
		}
		else if (smb && (name == "sign" || name == "seal"))
		{
			bool b = false;
			if (!ParseBool(has_value, value, b))
				return Fail(err, "sign and seal take no value (or 1 or 0)");
			(name == "sign" ? cfg.sign : cfg.seal) = b;
		}
		else if (!smb && name == "version")
		{
			if (!ParseInt(value, 3, 4, n))
				return Fail(err, "version must be 3 or 4");
			cfg.nfs_version = (int)n;
		}
		else if (!smb && (name == "nfsport" || name == "mountport"))
		{
			if (!ParseInt(value, 1, 65535, n))
				return Fail(err, "nfsport and mountport must be numbers from 1 to 65535");
			(name == "nfsport" ? cfg.nfsport : cfg.mountport) = (int)n;
		}
		else if (!smb && (name == "uid" || name == "gid"))
		{
			if (!ParseInt(value, 0, 999999999, n))
				return Fail(err, "uid and gid must be numbers");
			(name == "uid" ? cfg.uid : cfg.gid) = (int)n;
		}
		else
			return Fail(err, smb ? "an option that smb:// addresses do not have" : "an option that nfs:// addresses do not have");
	}
	return true;
}

std::string HostPort(const ShareConfig& c)
{
	return c.port ? c.host + ":" + std::to_string(c.port) : c.host;
}

std::string Scrub(std::string text, const std::string& password)
{
	if (password.empty())
		return text;
	for (const std::string& needle : {password, PctEncodeAll(password)})
	{
		size_t pos = 0;
		while ((pos = text.find(needle, pos)) != std::string::npos)
		{
			text.replace(pos, needle.size(), "***");
			pos += 3;
		}
	}
	return text;
}
} // namespace

bool netfs_parse_share(const std::string& address, ShareConfig& out, std::string& err)
{
	ShareConfig cfg;
	// The address is one word; a trailing "# note" is allowed, anything else is more likely a space typed
	// into a password or folder name, which would silently cut it short.
	size_t sp = address.find_first_of(" \t");
	std::string addr = address.substr(0, sp);
	if (sp != std::string::npos)
	{
		size_t r = address.find_first_not_of(" \t", sp);
		if (r != std::string::npos && address[r] != '#')
			return Fail(err, "text after the address (write a space as %20)");
	}

	std::string rest;
	std::string lower = Lower(addr.substr(0, 6));
	if (lower == "smb://")
		cfg.scheme = Scheme::Smb;
	else if (lower == "nfs://")
		cfg.scheme = Scheme::Nfs;
	else
		return Fail(err, "the address must start with smb:// or nfs://");
	rest = addr.substr(6);
	bool smb = cfg.scheme == Scheme::Smb;

	std::string query;
	size_t q = rest.find('?');
	if (q != std::string::npos)
	{
		query = rest.substr(q + 1);
		rest.resize(q);
	}
	size_t slash = rest.find('/');
	if (slash == std::string::npos)
		return Fail(err, smb ? "the share name is missing" : "the export path is missing");
	std::string authority = rest.substr(0, slash), path = rest.substr(slash);

	std::string hostport = authority;
	size_t at = authority.find('@');
	if (at != std::string::npos)
	{
		if (authority.find('@', at + 1) != std::string::npos)
			return Fail(err, "more than one @ (write @ inside a name as %40)");
		if (!smb)
			return Fail(err, "nfs:// addresses take no user name");
		std::string userinfo = authority.substr(0, at);
		hostport = authority.substr(at + 1);
		if (userinfo.find(':') != std::string::npos)
			return Fail(err, "put the password in ?password=, not user:password@");
		size_t semi = userinfo.find(';');
		if (semi != std::string::npos)
		{
			if (!DecodeField(userinfo.substr(0, semi), cfg.domain, "domain", err))
				return false;
			userinfo = userinfo.substr(semi + 1);
		}
		if (!DecodeField(userinfo, cfg.user, "user name", err))
			return false;
	}
	if (!ParseHostPort(hostport, cfg, err))
		return false;

	std::vector<std::string> segs;
	if (!ParseSegments(path, smb, segs, err))
		return false;
	if (smb)
	{
		if (segs.empty())
			return Fail(err, "the share name is missing");
		cfg.share = segs[0];
		for (size_t i = 1; i < segs.size(); i++)
			cfg.path += (i > 1 ? "/" : "") + segs[i];
	}
	else
	{
		if (segs.empty())
			return Fail(err, "the export path is missing");
		for (size_t i = 0; i < segs.size(); i++)
			cfg.path += (i ? "/" : "") + segs[i];
	}

	if (!ParseQuery(query, cfg, err))
		return false;
	out = cfg;
	return true;
}

NetfsShareFile netfs_parse_shares(const std::string& text)
{
	NetfsShareFile f;
	std::vector<std::string> mounts;
	std::vector<int> mount_line;
	size_t pos = 0;
	if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
		pos = 3;
	for (int line_no = 1; pos <= text.size(); line_no++)
	{
		size_t e = text.find('\n', pos);
		if (e == std::string::npos)
			e = text.size();
		std::string line = text.substr(pos, e - pos);
		pos = e + 1;
		size_t a = line.find_first_not_of(" \t\r");
		if (a == std::string::npos || line[a] == '#')
			continue;
		size_t b = line.find_last_not_of(" \t\r");
		line = line.substr(a, b - a + 1);

		std::string prefix = "shares.txt line " + std::to_string(line_no) + ": ";
		ShareConfig cfg;
		std::string err;
		if (!netfs_parse_share(line, cfg, err))
		{
			f.notices.push_back(prefix + err);
			continue;
		}
		std::string mp = netfs_mount_point(cfg);
		bool dup = false;
		for (size_t i = 0; i < mounts.size(); i++)
			if (mounts[i] == mp)
			{
				f.notices.push_back(prefix + "same folder as line " + std::to_string(mount_line[i]) + ", ignored");
				dup = true;
			}
		if (dup)
			continue;
		if ((int)f.shares.size() >= kNetfsMaxShares)
		{
			f.notices.push_back(prefix + "only " + std::to_string(kNetfsMaxShares) + " shares are used, this one is ignored");
			continue;
		}
		mounts.push_back(mp);
		mount_line.push_back(line_no);
		f.shares.push_back(cfg);
	}
	return f;
}

std::string netfs_shares_template()
{
	return "# Snes9x PS5 network shares: one address per line, up to 8. Games are read from the share;\n"
		   "# saves, states and covers stay on the console. A line that starts with # is a comment.\n"
		   "# Passwords are kept here in plain text and can be read over FTP.\n"
		   "#\n"
		   "#   smb://192.168.1.50/games/snes                            guest access\n"
		   "#   smb://jason@192.168.1.50/games/snes?password=secret      user and password\n"
		   "#   smb://WORKGROUP;jason@nas/games?password=x&vers=3&sign   domain, SMB 3, signing\n"
		   "#   nfs://192.168.1.50/export/snes                           NFS version 3\n"
		   "#   nfs://nas/export/snes?version=4&uid=1000&gid=100\n"
		   "#\n"
		   "# Use the server's IP address. Inside a user name, password or folder, write & = % # @ ; / ? and\n"
		   "# spaces as %XX (UTF-8 bytes): the password p@ss&word=1 is written p%40ss%26word%3D1.\n";
}

std::string netfs_mount_point(const ShareConfig& c)
{
	if (c.scheme == Scheme::Smb)
		return "/smb/" + HostPort(c) + "/" + c.share + (c.path.empty() ? "" : "/" + c.path);
	return "/nfs/" + HostPort(c) + "/" + c.path;
}

std::string netfs_redact(const ShareConfig& c)
{
	bool smb = c.scheme == Scheme::Smb;
	std::string s = smb ? "smb://" : "nfs://";
	if (smb && (!c.user.empty() || !c.domain.empty()))
	{
		if (!c.domain.empty())
			s += PctEncodeDisplay(c.domain) + ";";
		s += PctEncodeDisplay(c.user) + "@";
	}
	s += HostPort(c) + "/";
	std::string loc = smb ? c.share + (c.path.empty() ? "" : "/" + c.path) : c.path;
	size_t i = 0;
	while (i <= loc.size())
	{
		size_t e = loc.find('/', i);
		if (e == std::string::npos)
			e = loc.size();
		s += (i ? "/" : "") + PctEncodeDisplay(loc.substr(i, e - i));
		i = e + 1;
	}
	std::string opts;
	auto add = [&](const std::string& o) { opts += (opts.empty() ? "?" : "&") + o; };
	if (smb)
	{
		if (!c.vers.empty())
			add("vers=" + c.vers);
		if (c.sign)
			add("sign");
		if (c.seal)
			add("seal");
	}
	else
	{
		if (c.nfs_version != 3)
			add("version=" + std::to_string(c.nfs_version));
		if (c.nfsport)
			add("nfsport=" + std::to_string(c.nfsport));
		if (c.mountport)
			add("mountport=" + std::to_string(c.mountport));
		if (c.uid >= 0)
			add("uid=" + std::to_string(c.uid));
		if (c.gid >= 0)
			add("gid=" + std::to_string(c.gid));
	}
	return s + opts;
}

std::string netfs_error_text(const Status& st, const ShareConfig& c)
{
	bool smb = c.scheme == Scheme::Smb;
	std::string host = HostPort(c);
	std::string where = smb ? "share " + c.share : "export " + c.path;
	std::string t;
	switch (st.kind)
	{
	case ErrKind::None:
		t = "";
		break;
	case ErrKind::NoAnswer:
		t = host + " did not answer. Check that it is on and on the same network.";
		break;
	case ErrKind::Refused:
		t = host + " refused the connection. Check that " + (smb ? "file sharing (SMB)" : "NFS") + " is switched on there.";
		break;
	case ErrKind::Closed:
		t = host + " closed the connection.";
		break;
	case ErrKind::NameNotFound:
		t = "No server named " + host + " was found. Use its IP address.";
		break;
	case ErrKind::AccessDenied:
		t = host + " denied access. Check the user name and password.";
		break;
	case ErrKind::ShareNotFound:
	{
		t = std::string("The ") + where + " is not on " + host + ".";
		if (!st.available.empty())
		{
			t += smb ? " Shares there:" : " Exports there:";
			for (size_t i = 0; i < st.available.size(); i++)
				t += (i ? ", " : " ") + st.available[i];
			t += ".";
		}
		break;
	}
	case ErrKind::FolderNotFound:
		t = "The folder " + (c.path.empty() ? c.share : c.path) + " is not on " + host + ".";
		break;
	case ErrKind::Smb1Only:
		t = host + " only speaks SMB1, which is not supported. Turn on SMB 2 or 3 on the server.";
		break;
	case ErrKind::SigningRequired:
		t = host + " requires signing. Add ?sign to the address.";
		break;
	case ErrKind::NotBuilt:
		t = st.text.empty() ? "This protocol is not built into this release." : st.text + ".";
		break;
	case ErrKind::Aborted:
		t = "The connection to " + host + " was lost.";
		break;
	case ErrKind::Other:
		t = "Could not use " + host + (st.text.empty() ? "." : ": " + st.text);
		break;
	}
	return Scrub(t, c.password);
}
