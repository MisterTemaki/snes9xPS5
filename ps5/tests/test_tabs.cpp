// The shelf's tab rules (frontend/fe_tabs.h): which game stands on which tab, which tab opens, how L1 / R1 step.
// SPDX-License-Identifier: MIT

#include "fe_tabs.h"

#include <cstdio>
#include <string>

using namespace fe;

static int g_fail = 0;
#define CHECK(c)                                                          \
	do                                                                    \
	{                                                                     \
		if (!(c))                                                         \
		{                                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
			g_fail++;                                                     \
		}                                                                 \
	} while (0)

static GameInfo Game(const char* path, bool usb, bool net)
{
	GameInfo g;
	g.path = path;
	g.on_usb = usb;
	g.on_network = net;
	return g;
}

int main()
{
	using namespace tabs;

	// membership: a share wins over USB, USB over the console's drive
	CHECK(Of(Game("/data/a.sfc", false, false)) == Internal);
	CHECK(Of(Game("/mnt/usb0/a.sfc", true, false)) == Usb);
	CHECK(Of(Game("/smb/h/s/a.sfc", false, true)) == Network);

	std::vector<GameInfo> games = {Game("/smb/h/s/n.sfc", false, true), Game("/data/b.sfc", false, false), Game("/mnt/usb0/c.sfc", true, false),
								   Game("/data/a.sfc", false, false), Game("/smb/h/s/m.sfc", false, true)};
	int counts[kCount];
	Count(games, counts);
	CHECK(counts[Internal] == 2 && counts[Usb] == 1 && counts[Network] == 2);

	// the shelf list: each tab's games side by side, the order inside a tab kept
	OrderByTab(games);
	CHECK(games[0].path == "/data/b.sfc" && games[1].path == "/data/a.sfc");
	CHECK(games[2].path == "/mnt/usb0/c.sfc");
	CHECK(games[3].path == "/smb/h/s/n.sfc" && games[4].path == "/smb/h/s/m.sfc");
	const std::vector<int> net = View(games, Network);
	CHECK(net.size() == 2 && net[0] == 3 && net[1] == 4);
	CHECK(View(games, Usb).size() == 1);

	// USB is hidden while no game is on a USB drive; Internal and Network are always there
	const int none[kCount] = {0, 0, 0};
	CHECK(Shown(none, Internal) && !Shown(none, Usb) && Shown(none, Network));
	CHECK(Shown(counts, Usb));

	// the tab that opens: (counts, asked for, player chose it or a share may still come, a share is set up)
	const int local_only[kCount] = {3, 0, 0};
	const int net_only[kCount] = {0, 0, 4};
	const int usb_only[kCount] = {0, 2, 0};
	CHECK(Resolve(local_only, Internal, false, false) == Internal);
	CHECK(Resolve(local_only, Network, false, false) == Internal);  // nothing on the share and none set up: back to the games
	CHECK(Resolve(local_only, Network, true, true) == Network);     // a share is still coming, or the player chose it: stay
	CHECK(Resolve(net_only, Internal, false, true) == Network);     // only network games: open on them
	CHECK(Resolve(net_only, Internal, true, true) == Internal);     // unless the player chose the empty tab
	CHECK(Resolve(usb_only, Usb, false, false) == Usb);
	CHECK(Resolve(local_only, Usb, true, false) == Internal);       // the USB tab is gone with its last game, even if chosen
	CHECK(Resolve(none, Network, false, false) == Network);         // nothing anywhere: where it was asked for
	CHECK(Resolve(none, Internal, false, true) == Network);         // ... but a share that is set up has something to say
	CHECK(Resolve(none, Internal, true, true) == Internal);         // (the player's own choice stays)
	CHECK(Resolve(none, Usb, false, false) == Internal);
	CHECK(Resolve(none, 7, false, false) == Internal);              // a damaged setting
	CHECK(Resolve(none, -1, false, false) == Internal);

	// L1 / R1 go round the shown tabs
	CHECK(Step(none, Internal, +1) == Network);
	CHECK(Step(none, Network, +1) == Internal);
	CHECK(Step(none, Internal, -1) == Network);
	CHECK(Step(counts, Internal, +1) == Usb);
	CHECK(Step(counts, Usb, +1) == Network);
	CHECK(Step(counts, Network, +1) == Internal);
	CHECK(Step(counts, Internal, -1) == Network);

	CHECK(std::string(Name(Internal)) == "Internal" && std::string(Name(Usb)) == "USB" && std::string(Name(Network)) == "Network");

	if (g_fail)
		return 1;
	printf("tabs: all checks passed\n");
	return 0;
}
