// Snes9x PS5: the picture, through libSceVideoOut (no GPU driver needed).
//
// The frontend draws into a linear 1920x1080 A8B8G8R8 surface (0xAABBGGRR in memory order R,G,B,A).
// Present() swizzles the changed part into one of two tiled scan-out buffers and flips it.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace ps5video
{
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

bool Init();
void Shutdown();

uint32_t* Surface(); // kWidth * kHeight pixels, pitch kWidth

// Copies the rectangle (x, y, w, h) of the surface (everything when w or h is 0) to the back buffer and
// flips it. wait_vsync: return once the flip happened (the frame pacing for 60 Hz games); otherwise the
// next Present waits for it instead, before it touches that buffer again.
void Present(int x, int y, int w, int h, bool wait_vsync);

inline uint32_t Rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}

// Fills / darkens part of the surface (clipped).
void FillRect(int x, int y, int w, int h, uint32_t color);
void DarkenRect(int x, int y, int w, int h); // halves every channel

// The SNES picture: how GFX.Screen (RGB565) is scaled into the surface.
enum class Aspect : int
{
	Ratio4x3 = 0, // 1440x1080, what a CRT showed
	PixelPerfect, // 8:7 square pixels at full height
	Integer, // largest whole multiple that fits (4x: 1024x896)
	Stretch, // 1920x1080
	Count
};
const char* AspectName(Aspect a);

struct Rect
{
	int x, y, w, h;
};

// Art around the picture (docs: .context/projects/snes9xps5/borders-spec.md). The picture goes in the border's window;
// the border itself is painted into the surface once, where the surface is cleared, so a normal frame still changes
// only the picture rectangle.
// One pixel of the border that lies inside the window: drawn over the picture every frame.
struct RimPixel
{
	uint32_t index; // y * kWidth + x
	uint32_t pre; // 0xAABBGGRR with A = 0: each colour already multiplied by alpha / 255
	uint32_t inv; // 255 - alpha
};
struct Border
{
	std::vector<uint32_t> backdrop; // kWidth * kHeight, 0xAABBGGRR, opaque. Window pixels 0xff000000, rim pixels already on black.
	Rect window;
	std::vector<RimPixel> rim; // every pixel inside `window` with alpha 1..255, sorted by index
};
// The border DrawSnes uses from the next frame on; null = none (today's black). Cheap: swaps a pointer.
void SetBorder(std::shared_ptr<const Border> border);

// Where the picture goes inside 'bounds' (the whole surface, or a border's window): centred, width kept even.
// 4:3 and 8:7 fit by the limiting side, or fill 'bounds' when it has that ratio within 3%; Integer is the largest whole
// multiple that fits; Stretch fills 'bounds'. src_w / src_h are the picture's size as the core draws it.
Rect ComputeRect(int src_w, int src_h, Aspect aspect, const Rect& bounds);

// Scales a w x h RGB565 picture (pitch in bytes) into the surface (nearest neighbour); returns the
// rectangle that changed so the caller can Present just that. 'scanlines' darkens the last output row of
// every source line, CRT style, when the scale is 3x or more.
// shader: a ps5crt::Shader (0 = none: the plain scaler, with 'scanlines'); with one, the CRT shader draws the picture
// instead (ProsperoCrt.h).
Rect DrawSnes(const uint16_t* src, int pitch_bytes, int w, int h, Aspect aspect, bool scanlines, int shader = 0);
// The shader's average drawing time since the last call, in ms (0 when no shader drew).
double TakeShaderMs();
// The last rectangle DrawSnes covered (to darken it for the menu, or to clear around it).
Rect LastSnesRect();
// Forget the cached geometry: the next DrawSnes clears the borders.
void InvalidateSnes();
} // namespace ps5video
