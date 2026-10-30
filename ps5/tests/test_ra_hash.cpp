// Snes9x PS5 (2.11): the RetroAchievements hash of a game file (frontend/fe_rahash.h), for tests/test_ra_hash.py.
//
//   test-ra-hash <file>...    prints "<hash> <file>" for each, or "FAIL <why> <file>"
//
// SPDX-License-Identifier: MIT

#include "fe_rahash.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
	int failed = 0;
	for (int i = 1; i < argc; i++)
	{
		std::string hash, why;
		if (fe::rahash::HashFile(argv[i], &hash, &why))
			printf("%s %s\n", hash.c_str(), argv[i]);
		else
		{
			printf("FAIL %s %s\n", why.c_str(), argv[i]);
			failed++;
		}
	}
	return failed ? 1 : 0;
}
