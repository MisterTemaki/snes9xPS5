// Snes9x PS5 (2.11): RetroAchievements (retroachievements.org), with its official client library rcheevos (rc_client,
// third_party/rcheevos, MIT).
//
//   - Account: Settings, RetroAchievements: log in with the user name and the password (on the on-screen keyboard; the
//     password is masked, sent once and never stored); the server's token is kept in
//     /data/snes9x/retroachievements/login.txt (owner-only), and the next start logs in with it quietly. Log out deletes it.
//   - Without a login nothing changes and nothing is sent: no request, no thread.
//   - A game: identified by its hash (fe_rahash.h); its achievements, leaderboards and rich presence come from the server;
//     rc_client_do_frame runs after every emulated frame, rc_client_idle about once a second while the game is paused.
//   - Notifications are small boxes drawn over the picture (DrawOverlay): they never pause or block the game.
//   - Hardcore mode (on by default, as RetroAchievements asks): loading states and cheats are off (and the menus grey
//     them), Transparency stays on (RetroAchievements forbids turning Snes9x's transparency off), and the pause menu
//     follows rc_client_can_pause. Turning it on in a game resets the game; off is allowed at any time. Fast forward
//     stays (RetroAchievements allows it); the port has no rewind, slow motion or frame advance.
//   - Network: every request goes through the helper (fe_ranet.h): the app's own HTTPS fails out of its sandbox.
//
// Everything here runs on the main thread (the frame loop and the menus): rc_client is never called from another one.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "ProsperoVideo.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ra
{
// At start, once /data is there: with a saved login, a quiet login with its token (nothing on the screen; a bad or
// expired token shows in Settings). Without one, nothing at all.
void Init();
// At the end: the request thread stopped.
void Shutdown();
// Once a frame on every screen (the shelf, menus, the game): answers of the server handed to rc_client; rc_client_idle
// about once a second while no game frame runs.
void Pump();

// ---- the account
enum class Login
{
	None, // no account: nothing happens
	Pending, // a login is under way
	LoggedIn,
	Failed, // the last login failed (Problem() says why); for want of a network (or with an old helper) the saved token is
	        // tried again by itself (Pump) and at a game's start
};
Login LoginState();
// 2.11 review: an account that counts this session: logged in, logging in, or a login that failed for want of a network
// (tried again by itself) or because of an old helper (said at each game). One the server refused counts as none, except
// in Settings, which says why.
bool AccountActive();
std::string UserName();
uint32_t Points(); // the hardcore score, or the softcore one with hardcore off
// Why the account isn't logged in (or what went wrong with the network), translated; "" when all is well.
std::string Problem();
// Starts a login with a password, which is sent once (through the helper) and never kept: `password` is wiped.
void BeginLogin(const std::string& user, std::string& password);
void Logout();

// ---- hardcore mode
bool HardcoreSetting();
// The setting. On in a game whose achievements are loaded (or loading): the game is reset, as RetroAchievements requires.
// false (and `refused` says why, translated) when it can't be turned on in this game (snes9x.conf forced PAL at its start).
bool SetHardcore(bool on, std::string* refused = nullptr);
// The restrictions apply now: hardcore on, an account, and a game whose achievements are loaded or still loading.
bool HardcoreActive();
// 2.11 second review: the running game is softcore although the setting is on (its set came only after it started:
// a login that went through late); the pause menu says so.
bool SessionSoftcore();
// Whether the pause menu may open now (rc_client_can_pause in hardcore; always outside it); seconds to wait when not.
bool CanPause(int* seconds_left);

// ---- the game (fe_emu.cpp calls these)
// After the ROM loaded: the hash, then the set from the server. `patch_tried`: Snes9x ran a soft patch's reader on the
// ROM (memmap.cpp, Memory.PatchTried); `patched` then holds the bytes it left (S9xRomPatched), which are hashed instead of
// the file -- and without them the game is unknown: a patched game is never hashed as its file.
void GameStarted(const std::string& rom_path, bool patch_tried = false, const std::vector<uint8_t>* patched = nullptr);
void GameClosed();
void Frame(); // after every emulated frame
void OnReset(); // the game was reset (rc_client_reset)
// rc_client's progress (hit counts...) for a save state, and back; Load with no data (a state saved without it) resets it.
bool SaveProgress(std::vector<uint8_t>* out);
void LoadProgress(const std::vector<uint8_t>* data);
// The game's achievements are loaded (the pause menu's Achievements list has something to show)
bool GameHasSet();
// The game's title on RetroAchievements ("" before its set is loaded)
std::string GameTitle();
// "3 of 10 achievements unlocked" (translated), or why there are none ("Unknown game"...), for the pause menu
std::string GameSummary();

// ---- what the player sees
// Over the game's picture, after each drawn frame: the notifications (bottom left), the trackers and challenge
// indicators (top right). Only inside `picture`, which the next frame draws over again.
void DrawOverlay(const ps5video::Rect& picture);
// Something to draw (or drawn last time, to be cleared): the shelf redraws while this is true (2.11 review: notifications
// raised on the shelf, such as an unlock sent once the network is back, are shown there)
bool OverlayBusy();

// The achievement list, for the pause menu's Achievements screen
struct Item
{
	bool section = false; // a heading ("Locked", "Unlocked"...)
	std::string title, description;
	uint32_t points = 0;
	bool unlocked = false;
	std::string progress; // "12/100" when rc_client measures it
	float percent = 0;
};
std::vector<Item> AchievementItems();

// The User-Agent sent with every request: "Snes9xPS5/<version> (PlayStation 5) Snes9x/<core> rcheevos/<version>"
std::string UserAgent();
} // namespace ra
