/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file 40bpp_optimized.cpp Implementation of the optimized 40 bpp blitter. */

#include "../stdafx.h"
#include "../zoom_func.h"
#include "../settings_type.h"
#include "../video/video_driver.hpp"
#include "../palette_func.h"
#include "40bpp_anim.hpp"
#include "common.hpp"

#include "../table/sprites.h"

#include "../safeguards.h"


/** Instantiation of the 40bpp with animation blitter factory. */
static FBlitter_40bppAnim iFBlitter_40bppAnim;


void Blitter_40bppAnim::SetPixel(void *video, int x, int y, uint16_t colour)
{
	if (_screen_disable_anim) {
		Blitter_32bppOptimized::SetPixel(video, x, y, colour);
	} else {
		size_t y_offset = static_cast<size_t>(y) * _screen.pitch;
		*((Colour *)video + x + y_offset) = _black_colour;

		VideoDriver::GetInstance()->GetAnimBuffer()[((uint32_t *)video - (uint32_t *)_screen.dst_ptr) + x + y_offset] = colour;
	}
}

void Blitter_40bppAnim::SetPixel32(void *video, int x, int y, uint8_t colour, uint32_t colour32)
{
	if (_screen_disable_anim) {
		Blitter_32bppOptimized::SetPixel32(video, x, y, colour, colour32);
	} else {
		size_t y_offset = static_cast<size_t>(y) * _screen.pitch;
		*((Colour *)video + x + y_offset) = colour32;

		VideoDriver::GetInstance()->GetAnimBuffer()[((uint32_t *)video - (uint32_t *)_screen.dst_ptr) + x + y_offset] = 0;
	}
}

void Blitter_40bppAnim::SetRect(void *video, int x, int y, const uint8_t *colours, uint lines, uint width, uint pitch)
{
	if (_screen_disable_anim) {
		Blitter_32bppOptimized::SetRect(video, x, y, colours, lines, width, pitch);
	} else {
		Colour *dst = (Colour *)video + x + y * _screen.pitch;
		uint8_t *dstanim = ((uint32_t *)dst - (uint32_t *)_screen.dst_ptr) + VideoDriver::GetInstance()->GetAnimBuffer();
		do {
			memset_colour(dst, _black_colour, width);
			memcpy(dstanim, colours, width * sizeof(uint8_t));
			dst += _screen.pitch;
			dstanim += _screen.pitch;
			colours += pitch;
		} while (--lines);
	}
}

void Blitter_40bppAnim::SetRect32(void *video, int x, int y, const uint32_t *colours, uint lines, uint width, uint pitch)
{
	if (_screen_disable_anim) {
		Blitter_32bppOptimized::SetRect32(video, x, y, colours, lines, width, pitch);
	} else {
		Colour *dst = (Colour *)video + x + y * _screen.pitch;
		uint8_t *dstanim = ((uint32_t *)dst - (uint32_t *)_screen.dst_ptr) + VideoDriver::GetInstance()->GetAnimBuffer();
		do {
			memcpy((uint32_t *)dst, colours, width * sizeof(uint32_t));
			memset(dstanim, 0, width * sizeof(uint8_t));
			dst += _screen.pitch;
			dstanim += _screen.pitch;
			colours += pitch;
		} while (--lines);
	}
}

void Blitter_40bppAnim::SetRectNoD7(void *video, int x, int y, const uint8_t *colours, uint lines, uint width, uint pitch)
{
	if (_screen_disable_anim) {
		Blitter_32bppOptimized::SetRectNoD7(video, x, y, colours, lines, width, pitch);
	} else {
		Colour *dst = (Colour *)video + x + y * _screen.pitch;
		uint8_t *dstanim = ((uint32_t *)dst - (uint32_t *)_screen.dst_ptr) + VideoDriver::GetInstance()->GetAnimBuffer();
		do {
			for (uint i = 0; i < width; i++) {
				if (colours[i] != 0xD7) {
					dst[i] = _black_colour;
					dstanim[i] = colours[i];
				}
			}
			dst += _screen.pitch;
			dstanim += _screen.pitch;
			colours += pitch;
		} while (--lines);
	}
}

void Blitter_40bppAnim::DrawRect(void *video, int width, int height, uint16_t colour)
{
	if (_screen_disable_anim) {
		/* This means our output is not to the screen, so we can't be doing any animation stuff, so use our parent DrawRect() */
		Blitter_32bppOptimized::DrawRect(video, width, height, colour);
		return;
	}

	assert(VideoDriver::GetInstance()->GetAnimBuffer() != nullptr);
	uint8_t *anim_line = ((uint32_t *)video - (uint32_t *)_screen.dst_ptr) + VideoDriver::GetInstance()->GetAnimBuffer();

	do {
		memset_colour((Colour *)video, _black_colour, width);
		memset(anim_line, colour, width);

		video = (uint32_t *)video + _screen.pitch;
		anim_line += _screen.pitch;
	} while (--height);
}

void Blitter_40bppAnim::DrawRectAt(void *video, int x, int y, int width, int height, uint8_t colour)
{
	this->Blitter_40bppAnim::DrawRect((Colour *)video + x + y * _screen.pitch, width, height, colour);
}

void Blitter_40bppAnim::DrawLine(void *video, int x, int y, int x2, int y2, int screen_width, int screen_height, uint16_t colour, int width, int dash)
{
	if (_screen_disable_anim) {
		/* This means our output is not to the screen, so we can't be doing any animation stuff, so use our parent DrawRect() */
		Blitter_32bppOptimized::DrawLine(video, x, y, x2, y2, screen_width, screen_height, colour, width, dash);
		return;
	}

	assert(VideoDriver::GetInstance()->GetAnimBuffer() != nullptr);
	uint8_t *anim = ((uint32_t *)video - (uint32_t *)_screen.dst_ptr) + VideoDriver::GetInstance()->GetAnimBuffer();

	this->DrawLineGeneric(x, y, x2, y2, screen_width, screen_height, width, dash, [=](int x, int y) {
		*((Colour *)video + x + y * _screen.pitch) = _black_colour;
		*(anim + x + y * _screen.pitch) = colour;
	});
}

/**
 * Draws a sprite to a (screen) buffer. It is templated to allow faster operation.
 *
 * @tparam mode blitter mode
 * @param bp further blitting parameters
 * @param zoom zoom level at which we are drawing
 */
template <BlitterMode mode>
inline void Blitter_40bppAnim::Draw(const Blitter::BlitterParams *bp, ZoomLevel zoom)
{
	const SpriteData *src = (const SpriteData *)bp->sprite;

	/* src_px : each line begins with uint32_t n = 'number of bytes in this line',
	 *          then n times is the Colour struct for this line */
	const Colour *src_px = (const Colour *)(src->data + src->offset[zoom][0]);
	/* src_n  : each line begins with uint32_t n = 'number of bytes in this line',
	 *          then interleaved stream of 'm' and 'n' channels. 'm' is remap,
	 *          'n' is number of bytes with the same alpha channel class */
	const uint16_t *src_n  = (const uint16_t *)(src->data + src->offset[zoom][1]);

	/* skip upper lines in src_px and src_n */
	for (uint i = bp->skip_top; i != 0; i--) {
		src_px = (const Colour *)((const uint8_t *)src_px + *(const uint32_t *)src_px);
		src_n = (const uint16_t *)((const uint8_t *)src_n + *(const uint32_t *)src_n);
	}

	/* skip lines in dst */
	Colour *dst = (Colour *)bp->dst + bp->top * bp->pitch + bp->left;
	assert(VideoDriver::GetInstance()->GetAnimBuffer() != nullptr);
	uint8_t *anim = VideoDriver::GetInstance()->GetAnimBuffer() + ((uint32_t *)bp->dst - (uint32_t *)_screen.dst_ptr) + bp->top * bp->pitch + bp->left;

	/* store so we don't have to access it via bp every time (compiler assumes pointer aliasing) */
	const uint8_t *remap = bp->remap;

	for (int y = 0; y < bp->height; y++) {
		/* next dst line begins here */
		Colour *dst_ln = dst + bp->pitch;
		uint8_t *anim_ln = anim + bp->pitch;

		/* next src line begins here */
		const Colour *src_px_ln = (const Colour *)((const uint8_t *)src_px + *(const uint32_t *)src_px);
		src_px++;

		/* next src_n line begins here */
		const uint16_t *src_n_ln = (const uint16_t *)((const uint8_t *)src_n + *(const uint32_t *)src_n);
		src_n += 2;

		/* we will end this line when we reach this point */
		Colour *dst_end = dst + bp->skip_left;

		/* number of pixels with the same alpha channel class */
		uint n;

		while (dst < dst_end) {
			n = *src_n++;

			if (src_px->a == 0) {
				dst += n;
				src_px++;
				src_n++;

				if (dst > dst_end) anim += dst - dst_end;
			} else {
				if (dst + n > dst_end) {
					uint d = dst_end - dst;
					src_px += d;
					src_n += d;

					dst = dst_end - bp->skip_left;
					dst_end = dst + bp->width;

					n = std::min<uint>(n - d, (uint)bp->width);
					goto draw;
				}
				dst += n;
				src_px += n;
				src_n += n;
			}
		}

		dst -= bp->skip_left;
		dst_end -= bp->skip_left;

		dst_end += bp->width;

		while (dst < dst_end) {
			n = std::min<uint>(*src_n++, (uint)(dst_end - dst));

			if (src_px->a == 0) {
				anim += n;
				dst += n;
				src_px++;
				src_n++;
				continue;
			}

			draw:;

			switch (mode) {
				case BlitterMode::ColourRemap:
				case BlitterMode::CrashRemap:
				case BlitterMode::ColourRemapWithBrightness:
					if (src_px->a == 255) {
						do {
							uint8_t m = GB(*src_n, 0, 8);
							/* In case the m-channel is zero, only apply the crash remap by darkening the RGB colour. */
							if (m == 0) {
								Colour c = *src_px;
								if (mode == BlitterMode::ColourRemapWithBrightness) c = AdjustBrightness(c, DEFAULT_BRIGHTNESS + bp->brightness_adjust);
								if (mode == BlitterMode::CrashRemap) c = this->MakeDark(c);
								*dst = mode == BlitterMode::CrashRemap ? this->MakeDark(*src_px) : *src_px;
								*anim = 0;
							} else {
								uint r = remap[m];
								if (r != 0) {
									if (mode == BlitterMode::ColourRemapWithBrightness) {
										*dst = Colour(GetColourBrightness(*src_px) + bp->brightness_adjust, 0, 0);
									} else {
										*dst = src_px->data;
									}
									*anim = r;
								}
							}
							anim++;
							dst++;
							src_px++;
							src_n++;
						} while (--n != 0);
					} else {
						do {
							uint8_t m = GB(*src_n, 0, 8);
							Colour b = this->RealizeBlendedColour(*anim, *dst);
							if (m == 0) {
								Colour c = *src_px;
								if (mode == BlitterMode::ColourRemapWithBrightness) c = AdjustBrightness(c, DEFAULT_BRIGHTNESS + bp->brightness_adjust);
								if (mode == BlitterMode::CrashRemap) c = this->MakeDark(c);
								*dst = this->ComposeColourRGBANoCheck(c.r, c.g, c.b, src_px->a, b);
								*anim = 0;
							} else {
								uint r = remap[m];
								if (r != 0) {
									Colour c = this->LookupColourInPalette(r);
									if (mode == BlitterMode::ColourRemapWithBrightness) c = AdjustBrightness(c, DEFAULT_BRIGHTNESS + bp->brightness_adjust);
									*dst = this->ComposeColourPANoCheck(c, src_px->a, b);
									*anim = 0; // Animation colours don't work with alpha-blending.
								}
							}
							anim++;
							dst++;
							src_px++;
							src_n++;
						} while (--n != 0);
					}
					break;

				case BlitterMode::BlackRemap:
					do {
						*anim++ = 0;
						*dst++ = _black_colour;
						src_px++;
						src_n++;
					} while (--n != 0);
					break;

				case BlitterMode::Transparent:
					/* Make the current colour a bit more black, so it looks like this image is transparent */
					src_n += n;
					if (src_px->a == 255) {
						src_px += n;
						do {
							/* If the anim buffer contains a color value, the image composition will
							 * only look at the RGB brightness value. As such, we can simply darken the
							 * RGB value to darken the anim color. */
							Colour b = *anim != 0 ? Colour(GetColourBrightness(*dst), 0, 0) : *dst;
							*dst = this->MakeTransparent(b, 3, 4);
							anim++;
							dst++;
						} while (--n != 0);
					} else {
						do {
							Colour b = this->RealizeBlendedColour(*anim, *dst);
							*dst = this->MakeTransparent(b, (256 * 4 - src_px->a), 256 * 4);
							*anim = 0; // Animation colours don't work with alpha-blending.
							anim++;
							dst++;
							src_px++;
						} while (--n != 0);
					}
					break;

				case BlitterMode::TransparentRemap:
					/* Apply custom transparency remap. */
					src_n += n;
					if (src_px->a != 0) {
						src_px += n;
						do {
							if (*anim != 0) {
								*anim = remap[*anim];
							} else {
								*dst = this->LookupColourInPalette(remap[GetNearestColourIndex(*dst)]);
								*anim = 0;
							}
							anim++;
							dst++;
						} while (--n != 0);
					} else {
						dst += n;
						anim += n;
						src_px += n;
					}
					break;

				default:
					if (src_px->a == 255) {
						do {
							*anim++ = GB(*src_n, 0, 8);
							Colour c = *src_px;
							if (mode == BlitterMode::NormalWithBrightness) {
								if (GB(*src_n, 0, 8) == 0) {
									c = AdjustBrightness(c, DEFAULT_BRIGHTNESS + bp->brightness_adjust);
								} else {
									c = Colour(GetColourBrightness(c) + bp->brightness_adjust, 0, 0);
								}
							}
							*dst++ = c;
							src_px++;
							src_n++;
						} while (--n != 0);
						break;
					} else {
						do {
							uint8_t m = GB(*src_n, 0, 8);
							Colour b = this->RealizeBlendedColour(*anim, *dst);

							if (m == 0) {
								Colour c = *src_px;
								if (mode == BlitterMode::NormalWithBrightness) c = AdjustBrightness(c, DEFAULT_BRIGHTNESS + bp->brightness_adjust);
								*dst = this->ComposeColourRGBANoCheck(c.r, c.g, c.b, c.a, b);
								*anim = 0;
							} else {
								Colour c = this->LookupColourInPalette(m);
								if (mode == BlitterMode::NormalWithBrightness) c = Colour(GetColourBrightness(c) + bp->brightness_adjust, 0, 0);
								*dst = this->ComposeColourPANoCheck(c, src_px->a, b);
								*anim = m;
							}

							anim++;
							dst++;
							src_px++;
							src_n++;
						} while (--n != 0);
					}
			}
		}

		dst = dst_ln;
		anim = anim_ln;
		src_px = src_px_ln;
		src_n  = src_n_ln;
	}
}

/**
 * Draws a sprite to a (screen) buffer. Calls adequate templated function.
 *
 * @param bp further blitting parameters
 * @param mode blitter mode
 * @param zoom zoom level at which we are drawing
 */
void Blitter_40bppAnim::Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom)
{
	assert(_screen.dst_ptr != nullptr);

	if (_screen_disable_anim || VideoDriver::GetInstance()->GetAnimBuffer() == nullptr) {
		/* This means our output is not to the screen, so we can't be doing any animation stuff, so use our parent Draw() */
		Blitter_32bppOptimized::Draw<true>(bp, mode, zoom);
		return;
	}

	switch (mode) {
		default: NOT_REACHED();
		case BlitterMode::Normal: Draw<BlitterMode::Normal>(bp, zoom); return;
		case BlitterMode::ColourRemap: Draw<BlitterMode::ColourRemap>(bp, zoom); return;
		case BlitterMode::Transparent: Draw<BlitterMode::Transparent>(bp, zoom); return;
		case BlitterMode::TransparentRemap: Draw<BlitterMode::TransparentRemap>(bp, zoom); return;
		case BlitterMode::CrashRemap: Draw<BlitterMode::CrashRemap>(bp, zoom); return;
		case BlitterMode::BlackRemap: Draw<BlitterMode::BlackRemap>(bp, zoom); return;
		case BlitterMode::NormalWithBrightness: Draw<BlitterMode::NormalWithBrightness>(bp, zoom); return;
		case BlitterMode::ColourRemapWithBrightness: Draw<BlitterMode::ColourRemapWithBrightness>(bp, zoom); return;
	}
}

void Blitter_40bppAnim::DrawColourMappingRect(void *dst, int width, int height, PaletteID pal)
{
	if (_screen_disable_anim) {
		/* This means our output is not to the screen, so we can't be doing any animation stuff, so use our parent DrawColourMappingRect() */
		Blitter_32bppOptimized::DrawColourMappingRect(dst, width, height, pal);
		return;
	}

	Colour *udst = (Colour *)dst;
	uint8_t *anim = VideoDriver::GetInstance()->GetAnimBuffer() + ((uint32_t *)dst - (uint32_t *)_screen.dst_ptr);

	if (pal == PALETTE_TO_TRANSPARENT) {
		/* If the anim buffer contains a color value, the image composition will
		 * only look at the RGB brightness value. As such, we can simply darken the
		 * RGB value to darken the anim color. */
		do {
			for (int i = 0; i != width; i++) {
				Colour b = *anim != 0 ? Colour(GetColourBrightness(*udst), 0, 0) : *udst;
				*udst = MakeTransparent(b, 154);
				udst++;
				anim++;
			}
			udst = udst - width + _screen.pitch;
			anim = anim - width + _screen.pitch;
		} while (--height);
	} else if (pal == PALETTE_NEWSPAPER) {
		const uint8_t *remap = GetNonSprite(pal, SpriteType::Recolour);
		do {
			for (int i = 0; i != width; i++) {
				if (*anim == 0) *udst = MakeGrey(*udst);
				*anim = remap[*anim];
				udst++;
				anim++;
			}
			udst = udst - width + _screen.pitch;
			anim = anim - width + _screen.pitch;
		} while (--height);
	} else {
		const uint8_t *remap = GetNonSprite(pal, SpriteType::Recolour);
		do {
			for (int i = 0; i != width; i++) {
				*anim = remap[*anim];
				anim++;
			}
			anim = anim - width + _screen.pitch;
		} while (--height);
	}
}

Sprite *Blitter_40bppAnim::Encode(const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	return this->EncodeInternal<false>(sprite, allocator);
}


void Blitter_40bppAnim::CopyFromBuffer(void *video, const void *src, int width, int height)
{
	assert(!_screen_disable_anim);
	assert(video >= _screen.dst_ptr && video <= (uint32_t *)_screen.dst_ptr + _screen.width + _screen.height * _screen.pitch);
	uint32_t *dst = (uint32_t *)video;
	const uint32_t *usrc = (const uint32_t *)src;

	uint8_t *anim_buf = VideoDriver::GetInstance()->GetAnimBuffer();
	if (anim_buf == nullptr) return;
	uint8_t *anim_line = ((uint32_t *)video - (uint32_t *)_screen.dst_ptr) + anim_buf;

	for (; height > 0; height--) {
		memcpy(dst, usrc, width * sizeof(uint32_t));
		usrc += width;
		dst += _screen.pitch;
		/* Copy back the anim-buffer */
		memcpy(anim_line, usrc, width * sizeof(uint8_t));
		usrc = (const uint32_t *)((const uint8_t *)usrc + width);
		anim_line += _screen.pitch;
	}
}

void Blitter_40bppAnim::CopyToBuffer(const void *video, void *dst, int width, int height)
{
	assert(!_screen_disable_anim);
	assert(video >= _screen.dst_ptr && video <= (uint32_t *)_screen.dst_ptr + _screen.width + _screen.height * _screen.pitch);
	uint32_t *udst = (uint32_t *)dst;
	const uint32_t *src = (const uint32_t *)video;

	uint8_t *anim_buf = VideoDriver::GetInstance()->GetAnimBuffer();
	if (anim_buf == nullptr) return;
	const uint8_t *anim_line = ((const uint32_t *)video - (uint32_t *)_screen.dst_ptr) + anim_buf;

	for (; height > 0; height--) {
		memcpy(udst, src, width * sizeof(uint32_t));
		src += _screen.pitch;
		udst += width;
		/* Copy the anim-buffer */
		memcpy(udst, anim_line, width * sizeof(uint8_t));
		udst = (uint32_t *)((uint8_t *)udst + width);
		anim_line += _screen.pitch;
	}
}

void Blitter_40bppAnim::CopyImageToBuffer(const void *video, void *dst, int width, int height, int dst_pitch)
{
	uint8_t *anim_buf = VideoDriver::GetInstance()->GetAnimBuffer();
	if (anim_buf == nullptr) {
		Blitter_32bppOptimized::CopyImageToBuffer(video, dst, width, height, dst_pitch);
		return;
	}

	uint32_t *udst = (uint32_t *)dst;
	const uint32_t *src = (const uint32_t *)video;
	const uint8_t *anim_line = ((const uint32_t *)video - (uint32_t *)_screen.dst_ptr) + anim_buf;

	for (; height > 0; height--) {
		for (int x = 0; x < width; x++) {
			udst[x] = this->RealizeBlendedColour(anim_line[x], src[x]).data;
		}
		src += _screen.pitch;
		anim_line += _screen.pitch;
		udst += dst_pitch;
	}
}

void Blitter_40bppAnim::ScrollBuffer(void *video, int left, int top, int width, int height, int scroll_x, int scroll_y)
{
	assert(!_screen_disable_anim);
	assert(video >= _screen.dst_ptr && video <= (uint32_t *)_screen.dst_ptr + _screen.width + _screen.height * _screen.pitch);
	uint8_t *anim_buf = VideoDriver::GetInstance()->GetAnimBuffer();
	uint8_t *dst, *src;

	/* We need to scroll the anim-buffer too */
	if (scroll_y > 0) {
		dst = anim_buf + left + (top + height - 1) * _screen.pitch;
		src = dst - scroll_y * _screen.pitch;

		/* Adjust left & width */
		if (scroll_x >= 0) {
			dst += scroll_x;
		} else {
			src -= scroll_x;
		}

		uint tw = width + (scroll_x >= 0 ? -scroll_x : scroll_x);
		uint th = height - scroll_y;
		for (; th > 0; th--) {
			memcpy(dst, src, tw * sizeof(uint8_t));
			src -= _screen.pitch;
			dst -= _screen.pitch;
		}
	} else {
		/* Calculate pointers */
		dst = anim_buf + left + top * _screen.pitch;
		src = dst - scroll_y * _screen.pitch;

		/* Adjust left & width */
		if (scroll_x >= 0) {
			dst += scroll_x;
		} else {
			src -= scroll_x;
		}

		/* the y-displacement may be 0 therefore we have to use memmove,
		 * because source and destination may overlap */
		uint tw = width + (scroll_x >= 0 ? -scroll_x : scroll_x);
		uint th = height + scroll_y;
		for (; th > 0; th--) {
			memmove(dst, src, tw * sizeof(uint8_t));
			src += _screen.pitch;
			dst += _screen.pitch;
		}
	}

	Blitter_32bppBase::ScrollBuffer(video, left, top, width, height, scroll_x, scroll_y);
}

size_t Blitter_40bppAnim::BufferSize(uint width, uint height)
{
	return (sizeof(uint32_t) + sizeof(uint8_t)) * width * height;
}

Blitter::PaletteAnimation Blitter_40bppAnim::UsePaletteAnimation()
{
	return Blitter::PaletteAnimation::VideoBackend;
}

bool Blitter_40bppAnim::NeedsAnimationBuffer()
{
	return true;
}
