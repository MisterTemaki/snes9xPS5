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

struct NetfsShareFile
{
	std::vector<netfs::ShareConfig> shares;
	std::vector<std::string> notices; // "shares.txt line 3: ..." (the line number, never the line's text)
};
// Parses the whole file: comments, blank lines, a UTF-8 BOM and CRLF are fine; at most kNetfsMaxShares
// shares; a second share with the same mount point is dropped with a notice.
NetfsShareFile netfs_parse_shares(const std::string& text);

// The commented template written when shares.txt does not exist.
std::string netfs_shares_template();

// The virtual folder the share appears under: /smb/<host>[:port]/<share>[/<folder>] or /nfs/<host>[:port]/<path>.
std::string netfs_mount_point(const netfs::ShareConfig& cfg);

// An address rebuilt from the parsed fields only: scheme, domain, user, host, port, share, folder and the
// non-secret options. Never edits the raw line, never has the password. Safe for logs and notices.
std::string netfs_redact(const netfs::ShareConfig& cfg);

// One plain sentence for a failed connect (or a lost connection), free of the password.
std::string netfs_error_text(const netfs::Status& st, const netfs::ShareConfig& cfg);
