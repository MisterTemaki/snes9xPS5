// Snes9x PS5 netfs: share addresses (shares.txt), their mount points, and text that is safe to show.
//
// File format and escaping rules: see the "Share addresses" section of network-shares-spec.md in
// thatboialex/.context (projects/snes9xps5). Everything shown to the player or written to a log goes through
// netfs_redact() or netfs_error_text(); neither contains the password, in any encoding.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "netfs_backend.h"

constexpr int kNetfsMaxShares = 8;

// Parses one address (no comment, no surrounding spaces). On success fills `cfg`. On failure returns false and
// `error` holds a plain-word reason that never contains any part of the address.
bool netfs_parse_share(const std::string& address, netfs::ShareConfig& cfg, std::string& error);

// One address line of the file, in order: what both the loader and the Network screen see.
struct NetfsShareLine
{
	int line = 0;       // 1-based, as the notices count
	bool ok = false;    // the address parses
	std::string error;  // plain words, when it does not
	netfs::ShareConfig cfg; // when ok
	bool used = false;  // ok, and neither a duplicate nor past kNetfsMaxShares
	std::string ignored; // why not used: "same folder as line 2, ignored", ...
};
// Comments, blank lines, a UTF-8 BOM and CRLF are skipped; every other line is an address line.
std::vector<NetfsShareLine> netfs_scan_shares(const std::string& text);

struct NetfsShareFile
{
	std::vector<netfs::ShareConfig> shares;
	std::vector<std::string> notices; // "shares.txt line 3: ..." (the line number, never the line's text)
};
// Parses the whole file: comments, blank lines, a UTF-8 BOM and CRLF are fine; at most kNetfsMaxShares
// shares; a second share with the same mount point is dropped with a notice.
NetfsShareFile netfs_parse_shares(const std::string& text);

// The address as shares.txt keeps it: every name escaped as %XX where needed, the password as ?password=.
// THE PASSWORD IS IN THE RESULT: it goes into the file and nowhere else (never a log, a notice or the screen).
// netfs_parse_share() reads it back to the same config.
std::string netfs_format_share(const netfs::ShareConfig& cfg);

// What the Network screen's fields hold, as typed: nothing escaped. The options the screen has no field for
// (nfsport, mountport) are carried through.
struct NetfsShareFields
{
	netfs::Scheme scheme = netfs::Scheme::Smb;
	std::string server;   // an IP address or a name, with :port when it is not the default
	std::string share;    // SMB: the share's name; NFS: not used
	std::string folder;   // SMB: the folder inside the share; NFS: the export (everything after the host)
	std::string user;     // SMB; empty = guest
	std::string domain;   // SMB, optional
	std::string password; // SMB, kept exactly as typed
	std::string vers;     // SMB: "" (negotiate) or a version libsmb2 knows
	bool sign = false;    // SMB
	bool seal = false;    // SMB
	int nfs_version = 3;  // NFS: 3 or 4
	std::string uid, gid; // NFS: digits, or empty for the library's default
	int nfsport = 0, mountport = 0;
};
NetfsShareFields netfs_fields_from_config(const netfs::ShareConfig& cfg);
// Builds the config. On failure returns false and `error` is one sentence about the field to fix; it never has
// the password in it.
bool netfs_config_from_fields(const NetfsShareFields& f, netfs::ShareConfig& out, std::string& error);

// Edits to the text of shares.txt that change one line and leave every other byte alone: comments, order,
// line endings and a BOM stay. `line` counts as in NetfsShareLine. A replaced line keeps its indent and a "# note"
// after the address. set with line <= 0 appends the address as a new line. Both return false when `line` is not
// an address line of `text`.
bool netfs_shares_text_set(std::string& text, int line, const std::string& address);
bool netfs_shares_text_remove(std::string& text, int line);

// The commented template written when shares.txt does not exist.
std::string netfs_shares_template();

// The virtual folder the share appears under: /smb/<host>[:port]/<share>[/<folder>] or /nfs/<host>[:port]/<path>.
std::string netfs_mount_point(const netfs::ShareConfig& cfg);

// An address rebuilt from the parsed fields only: scheme, domain, user, host, port, share, folder and the
// non-secret options. Never edits the raw line, never has the password. Safe for logs and notices.
std::string netfs_redact(const netfs::ShareConfig& cfg);

// One plain sentence for a failed connect (or a lost connection), free of the password.
std::string netfs_error_text(const netfs::Status& st, const netfs::ShareConfig& cfg);
