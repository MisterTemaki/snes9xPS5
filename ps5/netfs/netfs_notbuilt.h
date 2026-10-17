// Snes9x PS5 netfs: the backend the stubs return while a protocol is not built in.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "netfs_backend.h"

namespace netfs
{
class NotBuiltBackend final : public Backend
{
public:
	explicit NotBuiltBackend(const char* protocol) : m_text(std::string(protocol) + " is not built into this release") {}

	Status Connect(const ShareConfig&) override { return Status::Kind(ErrKind::NotBuilt, m_text); }
	void Disconnect() override {}
	Status Stat(const std::string&, Attr&) override { return Status::Errno(ENOTCONN); }
	Status List(const std::string&, std::vector<DirEntry>&) override { return Status::Errno(ENOTCONN); }
	Status Open(const std::string&, Handle&) override { return Status::Errno(ENOTCONN); }
	int64_t Pread(Handle, void*, size_t, uint64_t) override { return -ENOTCONN; }
	void Close(Handle) override {}
	void Abort() override {}

private:
	std::string m_text;
};
} // namespace netfs
