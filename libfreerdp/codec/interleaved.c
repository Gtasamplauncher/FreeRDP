/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Interleaved RLE Bitmap Codec
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 * Copyright 2016 Armin Novak <armin.novak@thincast.com>
 * Copyright 2016 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <freerdp/codec/interleaved.h>
#include <freerdp/log.h>

#define TAG FREERDP_TAG("codec")

#define UNROLL_BODY(_exp, _count)      \
	do                                 \
	{                                  \
		size_t x;                      \
		for (x = 0; x < (_count); x++) \
		{                              \
			do                         \
				_exp while (FALSE);    \
		}                              \
	} while (FALSE)

#define UNROLL_MULTIPLE(_condition, _exp, _count) \
	do                                            \
	{                                             \
		while ((_condition) >= _count)            \
		{                                         \
			UNROLL_BODY(_exp, _count);            \
			(_condition) -= _count;               \
		}                                         \
	} while (FALSE)

#define UNROLL(_condition, _exp)               \
	do                                         \
	{                                          \
		UNROLL_MULTIPLE(_condition, _exp, 16); \
		UNROLL_MULTIPLE(_condition, _exp, 4);  \
		UNROLL_MULTIPLE(_condition, _exp, 1);  \
	} while (FALSE)

/*
   RLE Compressed Bitmap Stream (RLE_BITMAP_STREAM)
   http://msdn.microsoft.com/en-us/library/cc240895%28v=prot.10%29.aspx
   pseudo-code
   http://msdn.microsoft.com/en-us/library/dd240593%28v=prot.10%29.aspx
*/

#define REGULAR_BG_RUN 0x00
#define MEGA_MEGA_BG_RUN 0xF0
#define REGULAR_FG_RUN 0x01
#define MEGA_MEGA_FG_RUN 0xF1
#define LITE_SET_FG_FG_RUN 0x0C
#define MEGA_MEGA_SET_FG_RUN 0xF6
#define LITE_DITHERED_RUN 0x0E
#define MEGA_MEGA_DITHERED_RUN 0xF8
#define REGULAR_COLOR_RUN 0x03
#define MEGA_MEGA_COLOR_RUN 0xF3
#define REGULAR_FGBG_IMAGE 0x02
#define MEGA_MEGA_FGBG_IMAGE 0xF2
#define LITE_SET_FG_FGBG_IMAGE 0x0D
#define MEGA_MEGA_SET_FGBG_IMAGE 0xF7
#define REGULAR_COLOR_IMAGE 0x04
#define MEGA_MEGA_COLOR_IMAGE 0xF4
#define SPECIAL_FGBG_1 0xF9
#define SPECIAL_FGBG_2 0xFA
#define SPECIAL_WHITE 0xFD
#define SPECIAL_BLACK 0xFE

#define BLACK_PIXEL 0x000000

typedef UINT32 PIXEL;

static const BYTE g_MaskSpecialFgBg1 = 0x03;
static const BYTE g_MaskSpecialFgBg2 = 0x05;

static const BYTE g_MaskRegularRunLength = 0x1F;
static const BYTE g_MaskLiteRunLength = 0x0F;

/**
 * Reads the supplied order header and extracts the compression
 * order code ID.
 */
static INLINE UINT32 ExtractCodeId(BYTE bOrderHdr)
{
	if ((bOrderHdr & 0xC0U) != 0xC0U)
	{
		/* REGULAR orders
		 * (000x xxxx, 001x xxxx, 010x xxxx, 011x xxxx, 100x xxxx)
		 */
		return bOrderHdr >> 5;
	}
	else if ((bOrderHdr & 0xF0U) == 0xF0U)
	{
		/* MEGA and SPECIAL orders (0xF*) */
		return bOrderHdr;
	}
	else
	{
		/* LITE orders
		 * 1100 xxxx, 1101 xxxx, 1110 xxxx)
		 */
		return bOrderHdr >> 4;
	}
}

/**
 * Extract the run length of a compression order.
 */
static INLINE UINT32 ExtractRunLength(UINT32 code, const BYTE* pbOrderHdr, const BYTE* pbEnd,
                                      UINT32* advance)
{
	UINT32 runLength = 0;
	UINT32 ladvance = 1;

	if (pbOrderHdr >= pbEnd)
		return 0;

	switch (code)
	{
		case REGULAR_FGBG_IMAGE:
			runLength = (*pbOrderHdr) & g_MaskRegularRunLength;

			if (runLength == 0)
			{
				if (pbOrderHdr + 1 >= pbEnd)
					return 0;
				runLength = (*(pbOrderHdr + 1)) + 1;
				ladvance += 1;
			}
			else
			{
				runLength = runLength * 8;
			}

			break;

		case LITE_SET_FG_FGBG_IMAGE:
			runLength = (*pbOrderHdr) & g_MaskLiteRunLength;

			if (runLength == 0)
			{
				if (pbOrderHdr + 1 >= pbEnd)
					return 0;

				runLength = (*(pbOrderHdr + 1)) + 1;
				ladvance += 1;
			}
			else
			{
				runLength = runLength * 8;
			}

			break;

		case REGULAR_BG_RUN:
		case REGULAR_FG_RUN:
		case REGULAR_COLOR_RUN:
		case REGULAR_COLOR_IMAGE:
			runLength = (*pbOrderHdr) & g_MaskRegularRunLength;

			if (runLength == 0)
			{
				/* An extended (MEGA) run. */
				if (pbOrderHdr + 1 >= pbEnd)
					return 0;

				runLength = (*(pbOrderHdr + 1)) + 32;
				ladvance += 1;
			}

			break;

		case LITE_SET_FG_FG_RUN:
		case LITE_DITHERED_RUN:
			runLength = (*pbOrderHdr) & g_MaskLiteRunLength;

			if (runLength == 0)
			{
				/* An extended (MEGA) run. */
				if (pbOrderHdr + 1 >= pbEnd)
					return 0;

				runLength = (*(pbOrderHdr + 1)) + 16;
				ladvance += 1;
			}

			break;

		case MEGA_MEGA_BG_RUN:
		case MEGA_MEGA_FG_RUN:
		case MEGA_MEGA_SET_FG_RUN:
		case MEGA_MEGA_DITHERED_RUN:
		case MEGA_MEGA_COLOR_RUN:
		case MEGA_MEGA_FGBG_IMAGE:
		case MEGA_MEGA_SET_FGBG_IMAGE:
		case MEGA_MEGA_COLOR_IMAGE:
			if (pbOrderHdr + 2 >= pbEnd)
				return 0;

			runLength = ((UINT16)pbOrderHdr[1]) | ((UINT16)(pbOrderHdr[2] << 8));
			ladvance += 2;
			break;
	}

	*advance = ladvance;
	return runLength;
}

static INLINE BOOL ensure_capacity(const BYTE* start, const BYTE* end, size_t size, size_t base)
{
	const size_t available = (uintptr_t)end - (uintptr_t)start;
	const BOOL rc = available >= size * base;
	return rc && (start <= end);
}

static INLINE void write_pixel_8(BYTE* _buf, BYTE _pix)
{
	*_buf = _pix;
}

static INLINE void write_pixel_24(BYTE* _buf, UINT32 _pix)
{
	(_buf)[0] = (BYTE)(_pix);
	(_buf)[1] = (BYTE)((_pix) >> 8);
	(_buf)[2] = (BYTE)((_pix) >> 16);
}

static INLINE void write_pixel_16(BYTE* _buf, UINT16 _pix)
{
	_buf[0] = _pix & 0xFF;
	_buf[1] = (_pix >> 8) & 0xFF;
}

#undef DESTWRITEPIXEL
#undef DESTREADPIXEL
#undef SRCREADPIXEL
#undef DESTNEXTPIXEL
#undef SRCNEXTPIXEL
#undef WRITEFGBGIMAGE
#undef WRITEFIRSTLINEFGBGIMAGE
#undef RLEDECOMPRESS
#undef RLEEXTRA
#undef WHITE_PIXEL
#define WHITE_PIXEL 0xFF
#define DESTWRITEPIXEL(_buf, _pix) write_pixel_8(_buf, _pix)
#define DESTREADPIXEL(_pix, _buf) _pix = (_buf)[0]
#define SRCREADPIXEL(_pix, _buf) _pix = (_buf)[0]
#define DESTNEXTPIXEL(_buf) _buf += 1
#define SRCNEXTPIXEL(_buf) _buf += 1
#define WRITEFGBGIMAGE WriteFgBgImage8to8
#define WRITEFIRSTLINEFGBGIMAGE WriteFirstLineFgBgImage8to8
#define RLEDECOMPRESS RleDecompress8to8
#define RLEEXTRA
#undef ENSURE_CAPACITY
#define ENSURE_CAPACITY(_start, _end, _size) ensure_capacity(_start, _end, _size, 1)
#include "include/bitmap.c"

#undef DESTWRITEPIXEL
#undef DESTREADPIXEL
#undef SRCREADPIXEL
#undef DESTNEXTPIXEL
#undef SRCNEXTPIXEL
#undef WRITEFGBGIMAGE
#undef WRITEFIRSTLINEFGBGIMAGE
#undef RLEDECOMPRESS
#undef RLEEXTRA
#undef WHITE_PIXEL
#define WHITE_PIXEL 0xFFFF
#define DESTWRITEPIXEL(_buf, _pix) write_pixel_16(_buf, _pix)
#define DESTREADPIXEL(_pix, _buf) _pix = ((UINT16*)(_buf))[0]
#define SRCREADPIXEL(_pix, _buf) _pix = (_buf)[0] | ((_buf)[1] << 8)
#define DESTNEXTPIXEL(_buf) _buf += 2
#define SRCNEXTPIXEL(_buf) _buf += 2
#define WRITEFGBGIMAGE WriteFgBgImage16to16
#define WRITEFIRSTLINEFGBGIMAGE WriteFirstLineFgBgImage16to16
#define RLEDECOMPRESS RleDecompress16to16
#define RLEEXTRA
#undef ENSURE_CAPACITY
#define ENSURE_CAPACITY(_start, _end, _size) ensure_capacity(_start, _end, _size, 2)
#include "include/bitmap.c"

#undef DESTWRITEPIXEL
#undef DESTREADPIXEL
#undef SRCREADPIXEL
#undef DESTNEXTPIXEL
#undef SRCNEXTPIXEL
#undef WRITEFGBGIMAGE
#undef WRITEFIRSTLINEFGBGIMAGE
#undef RLEDECOMPRESS
#undef RLEEXTRA
#undef WHITE_PIXEL
#define WHITE_PIXEL 0xFFFFFF
#define DESTWRITEPIXEL(_buf, _pix) write_pixel_24(_buf, _pix)
#define DESTREADPIXEL(_pix, _buf) _pix = (_buf)[0] | ((_buf)[1] << 8) | ((_buf)[2] << 16)
#define SRCREADPIXEL(_pix, _buf) _pix = (_buf)[0] | ((_buf)[1] << 8) | ((_buf)[2] << 16)
#define DESTNEXTPIXEL(_buf) _buf += 3
#define SRCNEXTPIXEL(_buf) _buf += 3
#define WRITEFGBGIMAGE WriteFgBgImage24to24
#define WRITEFIRSTLINEFGBGIMAGE WriteFirstLineFgBgImage24to24
#define RLEDECOMPRESS RleDecompress24to24
#define RLEEXTRA
#undef ENSURE_CAPACITY
#define ENSURE_CAPACITY(_start, _end, _size) ensure_capacity(_start, _end, _size, 3)
#include "include/bitmap.c"

struct S_BITMAP_INTERLEAVED_CONTEXT
{
	BOOL Compressor;

	UINT32 TempSize;
	BYTE* TempBuffer;

	wStream* bts;
};

BOOL interleaved_decompress(BITMAP_INTERLEAVED_CONTEXT* interleaved, const BYTE* pSrcData,
                            UINT32 SrcSize, UINT32 nSrcWidth, UINT32 nSrcHeight, UINT32 bpp,
                            BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep, UINT32 nXDst,
                            UINT32 nYDst, UINT32 nDstWidth, UINT32 nDstHeight,
                            const gdiPalette* palette)
{
	UINT32 scanline;
	UINT32 SrcFormat;
	UINT32 BufferSize;

	if (!interleaved || !pSrcData || !pDstData)
		return FALSE;

	switch (bpp)
	{
		case 24:
			scanline = nSrcWidth * 3;
			SrcFormat = PIXEL_FORMAT_BGR24;
			break;

		case 16:
			scanline = nSrcWidth * 2;
			SrcFormat = PIXEL_FORMAT_RGB16;
			break;

		case 15:
			scanline = nSrcWidth * 2;
			SrcFormat = PIXEL_FORMAT_RGB15;
			break;

		case 8:
			scanline = nSrcWidth;
			SrcFormat = PIXEL_FORMAT_RGB8;
			break;

		default:
			WLog_ERR(TAG, "Invalid color depth %" PRIu32 "", bpp);
			return FALSE;
	}

	BufferSize = scanline * nSrcHeight;

	if (BufferSize > interleaved->TempSize)
	{
		interleaved->TempBuffer = _aligned_realloc(interleaved->TempBuffer, BufferSize, 16);
		interleaved->TempSize = BufferSize;
	}

	if (!interleaved->TempBuffer)
		return FALSE;

	switch (bpp)
	{
		case 24:
			if (!RleDecompress24to24(pSrcData, SrcSize, interleaved->TempBuffer, scanline,
			                         nSrcWidth, nSrcHeight))
				return FALSE;

			break;

		case 16:
		case 15:
			if (!RleDecompress16to16(pSrcData, SrcSize, interleaved->TempBuffer, scanline,
			                         nSrcWidth, nSrcHeight))
				return FALSE;

			break;

		case 8:
			if (!RleDecompress8to8(pSrcData, SrcSize, interleaved->TempBuffer, scanline, nSrcWidth,
			                       nSrcHeight))
				return FALSE;

			break;

		default:
			return FALSE;
	}

	return freerdp_image_copy(pDstData, DstFormat, nDstStep, nXDst, nYDst, nDstWidth, nDstHeight,
	                          interleaved->TempBuffer, SrcFormat, scanline, 0, 0, palette,
	                          FREERDP_FLIP_VERTICAL);
}

BOOL interleaved_compress(BITMAP_INTERLEAVED_CONTEXT* interleaved, BYTE* pDstData, UINT32* pDstSize,
                          UINT32 nWidth, UINT32 nHeight, const BYTE* pSrcData, UINT32 SrcFormat,
                          UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc, const gdiPalette* palette,
                          UINT32 bpp)
{
	BOOL status;
	wStream* s;
	UINT32 DstFormat = 0;
	const size_t maxSize = 64 * 64 * 4;

	if (!interleaved || !pDstData || !pSrcData)
		return FALSE;

	if ((nWidth == 0) || (nHeight == 0))
		return FALSE;

	if (nWidth % 4)
	{
		WLog_ERR(TAG, "interleaved_compress: width is not a multiple of 4");
		return FALSE;
	}

	if ((nWidth > 64) || (nHeight > 64))
	{
		WLog_ERR(TAG,
		         "interleaved_compress: width (%" PRIu32 ") or height (%" PRIu32
		         ") is greater than 64",
		         nWidth, nHeight);
		return FALSE;
	}

	switch (bpp)
	{
		case 24:
			DstFormat = PIXEL_FORMAT_BGRX32;
			break;

		case 16:
			DstFormat = PIXEL_FORMAT_RGB16;
			break;

		case 15:
			DstFormat = PIXEL_FORMAT_RGB15;
			break;

		default:
			return FALSE;
	}

	if (!freerdp_image_copy(interleaved->TempBuffer, DstFormat, 0, 0, 0, nWidth, nHeight, pSrcData,
	                        SrcFormat, nSrcStep, nXSrc, nYSrc, palette, FREERDP_FLIP_NONE))
		return FALSE;

	s = Stream_New(pDstData, *pDstSize);

	if (!s)
		return FALSE;

	Stream_SetPosition(interleaved->bts, 0);

	if (freerdp_bitmap_compress(interleaved->TempBuffer, nWidth, nHeight, s, bpp, maxSize,
	                            nHeight - 1, interleaved->bts, 0) < 0)
		status = FALSE;
	else
		status = TRUE;

	Stream_SealLength(s);
	*pDstSize = (UINT32)Stream_Length(s);
	Stream_Free(s, FALSE);
	return status;
}

BOOL bitmap_interleaved_context_reset(BITMAP_INTERLEAVED_CONTEXT* interleaved)
{
	if (!interleaved)
		return FALSE;

	return TRUE;
}

BITMAP_INTERLEAVED_CONTEXT* bitmap_interleaved_context_new(BOOL Compressor)
{
	BITMAP_INTERLEAVED_CONTEXT* interleaved;
	interleaved = (BITMAP_INTERLEAVED_CONTEXT*)calloc(1, sizeof(BITMAP_INTERLEAVED_CONTEXT));

	if (interleaved)
	{
		interleaved->TempSize = 64 * 64 * 4;
		interleaved->TempBuffer = _aligned_malloc(interleaved->TempSize, 16);

		if (!interleaved->TempBuffer)
		{
			free(interleaved);
			WLog_ERR(TAG, "_aligned_malloc failed!");
			return NULL;
		}

		interleaved->bts = Stream_New(NULL, interleaved->TempSize);

		if (!interleaved->bts)
		{
			_aligned_free(interleaved->TempBuffer);
			free(interleaved);
			WLog_ERR(TAG, "Stream_New failed!");
			return NULL;
		}
	}

	return interleaved;
}

void bitmap_interleaved_context_free(BITMAP_INTERLEAVED_CONTEXT* interleaved)
{
	if (!interleaved)
		return;

	_aligned_free(interleaved->TempBuffer);
	Stream_Free(interleaved->bts, TRUE);
	free(interleaved);
}
