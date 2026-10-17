// Snes9x PS5 netfs: the NFS backend until libnfs is wired in. NETFS_NFS_SRC in the Makefile swaps this file
// for the real backend_nfs.cpp.
//
// SPDX-License-Identifier: MIT
#include "netfs_notbuilt.h"

namespace netfs
{
std::unique_ptr<Backend> MakeNfsBackend()
{
	return std::make_unique<NotBuiltBackend>("NFS");
}
} // namespace netfs
