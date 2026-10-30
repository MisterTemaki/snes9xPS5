// Snes9x PS5 (2.11): the app's RetroAchievements requests, made by the helper (ProsperoJailbreak.h,
// kCmdRetroAchievements) on a thread of the app's own, so the frame loop never waits for the network.
//
// The thread starts with the first request: without a RetroAchievements login no request is made and no thread runs.
// Answers are handed back on the main thread, by Pump(), which every screen calls once a frame: rc_client then only
// ever runs on the main thread (it reads the game's memory there too).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>

namespace ra::net
{
// How a request ended, as far as the transport goes
enum class Outcome
{
	Http, // the server answered (http, body)
	NoHelper, // no helper on its port
	OldHelper, // a helper that doesn't know the request: a 2.10 one ("send the 2.11 payload")
	Offline, // the helper reached no server (no network, a timeout)
	Refused, // the helper refused it (an address that isn't RetroAchievements', too big, busy)
};

struct Answer
{
	Outcome outcome = Outcome::NoHelper;
	int http = 0;
	std::string body;
	std::string why; // the helper's reason, for the log
};

using Done = std::function<void(const Answer&)>;

// Queues a request: a POST of `post` to `url` (a GET when post is empty). `done` runs later on the main thread, from
// Pump(), exactly once (unless Stop() came first). `what` names it in the log ("r=login2"); the body is never logged.
void Send(const std::string& url, const std::string& post, const std::string& user_agent, const std::string& what,
	Done done);

// 2.11 review: a picture kept in a cache file: the worker reads `cache_file` if it is there; else it fetches `url` (a GET
// through the helper) and, when a PNG comes back, saves it there (.part, then renamed). `done` gets the bytes (http 200)
// either way. No file is touched on the main thread.
void FetchCached(const std::string& url, const std::string& cache_file, const std::string& user_agent,
	const std::string& what, Done done);

// Main thread: runs the `done` of every request answered since the last call.
void Pump();

// Requests still queued or running.
int InFlight();

// Stops the thread (the request under way is cut at once); the answers not yet handed back are dropped.
void Stop();
} // namespace ra::net
