// Snes9x PS5 netfs: the SMB backend until libsmb2 is wired in. NETFS_SMB_SRC in the Makefile swaps this file
// for the real backend_smb.cpp.
//
// SPDX-License-Identifier: MIT
#include "netfs_notbuilt.h"

namespace netfs
{
std::unique_ptr<Backend> MakeSmbBackend()
{
	return std::make_unique<NotBuiltBackend>("SMB");
}
} // namespace netfs
