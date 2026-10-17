# Snes9x PS5: the SMB backend's tests (tests/smb/run.sh), included by ps5/Makefile.
# SPDX-License-Identifier: MIT

$(HOST_OUT)/smb-backend-test: tests/smb/smb_backend_test.cpp $(HOST_OUT)/port/netfs/backend_smb.o $(HOST_LIBSMB2_OBJ) netfs/netfs_backend.h
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) -Wall -Wextra -Werror -Wno-unused-parameter -Inetfs -o $@ $< \
		$(HOST_OUT)/port/netfs/backend_smb.o $(HOST_LIBSMB2_OBJ) -lpthread

smb-test: $(HOST_OUT)/smb-backend-test
	bash tests/smb/run.sh

test: smb-test

.PHONY: smb-test
