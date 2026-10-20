// Snes9x PS5 frontend: a border picture, from a PNG file to the pieces the renderer wants (ps5video::Border).
//
//   DecodePng     a 1920x1080 PNG (RGBA, RGB, or palette with tRNS), straight alpha
//   FindWindow    the clear hole around the centre of the picture
//   FitToAspect   stretches the art so its window has the shape of the chosen aspect ratio
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

// The shape the window takes for an aspect ratio setting (a ratio rw:rh); false for Integer scale, whose window is a size,
// not a shape (IntegerOpening), and for no setting at all (Aspect::Count). Pixel perfect is 8:7 (the NTSC picture).
bool OpeningRatio(ps5video::Aspect aspect, int* rw, int* rh);

// Where the window goes to take the shape rw:rh: the same window when it is within 3% of that shape (what the renderer
// calls "fills"); else the height is kept and the width follows the shape, or, for a wider shape, the width grows until a
// margin of the art is down to kMinMarginPct of its drawn width and the height gives way to the shape. The margins keep
// the proportions they had.
ps5video::Rect FitOpening(const ps5video::Rect& window, int rw, int rh);
constexpr int kMinMarginPct = 40;

// Where the window goes for Integer scale: exactly the largest whole multiple of the 256 x 224 picture that fits the drawn
// window (4x, 1024x896, for the usual 1398x1038), so the picture has no black around it; the margins stretch to fill the
// rest and keep the proportions they had. The same window when it is that size already, or too small for 1x. The renderer
// fills a window of this kind with a 239-line picture too (ComputeRect).
ps5video::Rect IntegerOpening(const ps5video::Rect& window);

// Refits the art so its window is 'to' (from FitOpening) instead of 'from'. The window is cut in five pieces per axis
// (kOpeningCorner pixels at each end stay as drawn, so rounded corners stay round; the middle follows) and nothing is
// blended across a cut. The side margins are not squeezed or stretched out of shape: next to the window kOpeningRim pixels
// of each stay as drawn; the rest, the panel (a cover's art), is scaled up evenly and cropped to the height when its margin
// grows (the left panel from the top, the right one from the bottom) and cut to its outer part when it shrinks. Top and
// bottom margins stretch with the window. Bilinear, with the colours weighted by alpha. false (img unchanged) when memory
// runs out.
constexpr int kOpeningCorner = 64;
constexpr int kOpeningRim = 24;
bool RefitImage(Image* img, const ps5video::Rect& from, const ps5video::Rect& to, std::string* why);

// The two together for the border of a game: 'window' is where FindWindow (or an .ini) put it; on return it is the window
// of the stretched art, or untouched when it has the shape already (or the aspect is Aspect::Count: the art as drawn, which
// only tests ask for). false only when memory runs out (img and window unchanged).
bool FitToAspect(Image* img, ps5video::Rect* window, ps5video::Aspect aspect, std::string* why);

// null with the reason when the image is not surface-sized, the rim is over kMaxRim or memory runs out.
std::shared_ptr<const ps5video::Border> BuildBorder(const Image& img, const ps5video::Rect& window, std::string* why);
} // namespace fe::border
