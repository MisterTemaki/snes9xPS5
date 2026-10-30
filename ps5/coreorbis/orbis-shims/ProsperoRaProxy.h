// Snes9x PS5 (2.11): the helper's side of the app's RetroAchievements requests (ProsperoJailbreak.h,
// kCmdRetroAchievements).
//
// The app can't make HTTPS requests once it is out of its sandbox (the console's libSceSsl fails there), so the helper
// makes them with its own HTTPS (fe_tlshttp.h): a POST to https://retroachievements.org/dorequest.php, or a GET of a
// badge picture on https://media.retroachievements.org. Nothing else: any other address is refused, so the helper is
// no open proxy. The helper's request loop only reads the request and queues it; a thread of its own (started at the
// first request: no RetroAchievements traffic, no thread, until a player logs in) talks to the server and answers on
// the app's connection.
//
// Nothing secret reaches a log: the request is logged by its kind ("r=login2"), never with its body, and the password
// and the token found in a body are redacted from every log line while the request runs (the token for good).
//
// SPDX-License-Identifier: MIT
#pragma once

#include "ProsperoJailbreak.h"

#include <cstdint>
#include <string>

namespace raproxy
{
// The helper's loop read the header of a kCmdRetroAchievements request from `fd` (the title already checked): this reads
// the body (until deadline_ms, CLOCK_MONOTONIC), checks the address and queues the request. It owns fd from here: the
// worker answers and closes it (or this does, at once, when the request is refused).
void Take(int fd, const jailbreak::Request& req, int64_t deadline_ms);

// The addresses the helper fetches: https://retroachievements.org/dorequest.php and https://media.retroachievements.org/
// (pictures). The host build's tests add $SNES9X_RA_TEST_HOST (http://127.0.0.1:<port>), and only there.
bool Allowed(const std::string& url);

// What a request is, for the log: "r=login2" (the API's name; nothing else of the body), or "picture <path>".
std::string Describe(const std::string& url, const std::string& post);

// The values of the p= (password) and t= (token) fields of a form body, as sent (URL-encoded) and decoded.
void SecretsOf(const std::string& post, std::string* password, std::string* token);
} // namespace raproxy
