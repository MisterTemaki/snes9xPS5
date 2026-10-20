// Snes9x PS5: the covers downloaded in the background, by the helper payload.
//
// On the console the app itself can't download once it is out of its sandbox (fe_prefetch.h: every HTTPS
// GET fails after the jailbreak), so 1.0 downloaded at the very start, before the jailbreak, up to 30 s at a
// time with the launch screen up, and restarted itself for new covers. The helper is a payload of its own
// that keeps running while the app runs. So it downloads them: the app writes covers/wanted.txt after each scan (and
// covers/priority.txt with the covers around the selection), the helper fetches them one by one into
// /data/snes9x/covers, and the shelf shows each cover as it lands. covers/progress.txt says how far it got.
//
// 2.3: the console's libSceSsl fails in the helper too (Genesis Plus GX PS5 1.5's helper on a PS5: every GET ->
// send 8095f00c in 20 ms; the helper runs outside any sandbox, like the app after its jailbreak), so the helper
// brings its own HTTPS: TlsHttp (fe_tlshttp.h, Mbed TLS with Mozilla's CA list, plain sockets).
//
// The same thread downloads the borders (fe_borderfetch.h): borders/wanted.txt and borders/priority.txt in the same format, the
// pictures into borders/games, borders/progress.txt. The border of the game being played goes first, then the covers
// around the selection, the other covers, and last the borders of the whole library (Settings, Download borders).
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_http.h"

#include <memory>
#include <string>

namespace fe
{
// The worker's HTTP client: the helper's TlsHttp (fe_tlshttp.cpp); a build without it (the app, which never runs
// the worker) gets the libSceHttp2 one.
std::unique_ptr<HttpClient> MakeCoverWorkerHttp();

// The helper's download loop, one round at a time: the helper's thread runs it (StartCoverWorker), the tests step it with a
// fake client. `covers_dir`: the covers folder (OrbisDir("covers")); `borders_dir`: the borders folder (OrbisDir("borders")),
// whose games/ folder gets the pictures.
class DownloadWorker
{
public:
	DownloadWorker(std::unique_ptr<HttpClient> http, const std::string& covers_dir, const std::string& borders_dir);
	~DownloadWorker();
	// Reloads the lists that changed, then fetches the next file wanted, if any. Returns how many milliseconds to wait before
	// the next round (0: go on at once).
	int Step();
	void Run(); // Step, forever

private:
	struct Impl;
	std::unique_ptr<Impl> m_i;
};

// The helper, once it serves requests: starts the download thread (once).
void StartCoverWorker();

// <folder>/progress.txt, as the worker writes it: files left, fetched since the helper started, and its state.
struct CoverProgress
{
	bool valid = false; // a progress file the worker updated in the last minute
	int left = 0;
	int fetched = 0;
	std::string state; // "busy", "idle", "offline"
};
CoverProgress ReadCoverProgress(); // covers/progress.txt
CoverProgress ReadBorderProgress(); // borders/progress.txt

std::string PriorityListPath();
} // namespace fe
