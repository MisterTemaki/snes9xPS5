// Snes9x PS5: the helper's own HTTPS client, for the covers it downloads in the background.
//
// On the console libSceSsl only works inside the app's sandbox: a helper (a payload; Genesis Plus GX PS5 1.5's
// logs) got 8095f00c from sceHttp2SendRequest for every GET, in 20 ms, as the app does after its jailbreak
// (fe_prefetch.h). So the helper does its own: plain sockets, the address from getaddrinfo (then libSceNet's
// resolver, then GitHub's published addresses for raw.githubusercontent.com), TLS 1.2/1.3 from Mbed TLS (third_party/mbedtls, Apache-2.0), the
// server's certificate checked against Mozilla's CA list (data/cacert.pem, from certifi, MPL-2.0) and its name.
// HTTP/1.1, with the connection kept between requests, redirects followed, chunked answers read.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_http.h"

#include <atomic>
#include <memory>

namespace fe
{
class TlsHttp : public HttpClient
{
public:
	TlsHttp();
	~TlsHttp() override;
	int Get(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes = 8u << 20) override;
	bool Offline() const override;
	// Closes the connection and asks the console for its network state again on the next Get (the TLS setup stays).
	void Term() override;
	// The bytes of the answer read so far go here while a Get runs (the updater's progress); nullptr: nowhere.
	void SetProgress(std::atomic<size_t>* bytes);

	struct Impl;

private:
	std::unique_ptr<Impl> m;
};
} // namespace fe
