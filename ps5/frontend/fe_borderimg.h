// Snes9x PS5 frontend: a border picture, from a PNG file to the pieces the renderer wants (ps5video::Border).
//
//   DecodePng     a 1920x1080 PNG (RGBA, RGB, or palette with tRNS), straight alpha
//   FindWindow    the clear hole around the centre of the picture
//   BuildBorder   the backdrop (the picture on black) and the rim (the window's soft edge)
//
// Nothing here touches the disk or the renderer: bytes in, a border out, so the tests feed it generated PNGs.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "ProsperoVideo.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fe::border
{
constexpr size_t kMaxFileBytes = 8u << 20;
constexpr int kClearAlpha = 16; // below this a pixel counts as part of the window
constexpr int kMinWindowW = 480, kMinWindowH = 270; // a window found in the picture, or given in an .ini
// The rim is drawn over the picture every frame: more pixels than this (about 20 lines across the window) is art that
// would cost frame time, so it is refused.
constexpr size_t kMaxRim = 400000;

struct Image
{
	int w = 0, h = 0;
	std::vector<uint32_t> px; // w * h, 0xAABBGGRR, straight (not premultiplied) alpha
};

// false with the reason (a sentence fragment for "[border] <file>: <reason>"): empty or over 8 MiB, not a PNG, not
// exactly 1920x1080, 16 bits per channel, damaged.
bool DecodePng(const uint8_t* data, size_t size, Image* out, std::string* why);

// The window of a picture: the bounding box of the clear region (alpha below kClearAlpha, 4-connected) around the
// centre pixel. false when the centre is not clear or the window is under kMinWindowW x kMinWindowH.
bool FindWindow(const Image& img, ps5video::Rect* window, std::string* why);

// A window given by hand (x,y,w,h): inside the surface and at least kMinWindowW x kMinWindowH.
bool WindowFits(const ps5video::Rect& window);
// A window given by hand is a hole by decree: every pixel of the picture inside it becomes alpha 0, so the rim is empty
// (an RGB picture has no hole of its own; a soft glass edge is lost).
void PunchWindow(Image* img, const ps5video::Rect& window);

// null with the reason when the image is not surface-sized, the rim is over kMaxRim or memory runs out.
std::shared_ptr<const ps5video::Border> BuildBorder(const Image& img, const ps5video::Rect& window, std::string* why);
} // namespace fe::border
