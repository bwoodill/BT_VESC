/*

Adafruit invests time and resources providing this open source code, please
support Adafruit & open-source hardware by purchasing products from Adafruit!

Copyright (c) 2013 Adafruit Industries.  All rights reserved.

Modifications Copyright Claroworks.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>

#include "Adafruit_GFX.h"
#include "glcdfont.c"

// Pointers are a peculiar case...typically 16-bit on AVR boards,
// 32 bits elsewhere.  Try to accommodate both...

#define pgm_read_pointer(addr) ((void *)pgm_read_word(addr))

inline GFXglyph * pgm_read_glyph_ptr(const GFXfont *gfxFont, uint8_t c)
{
    return &(((GFXglyph *) &gfxFont->glyph)[c]);
}

inline uint8_t * pgm_read_bitmap_ptr(const GFXfont *gfxFont)
{
    return (uint8_t *) &gfxFont->bitmap;
}

#ifndef _swap_int16_t
#define _swap_int16_t(a, b) { int16_t t = a; a = b; b = t; }
#endif

#define pgm_read_byte(addr) ((uint8_t) *(addr))


int16_t
  WIDTH = 8,          ///< This is the 'raw' display width - never changes
  HEIGHT = 8;         ///< This is the 'raw' display height - never changes
int16_t
  _width,         ///< Display width as modified by current rotation
  _height,        ///< Display height as modified by current rotation
  cursor_x,       ///< x location to start print()ing text
  cursor_y;       ///< y location to start print()ing text
uint16_t
  textcolor,      ///< 16-bit background color for print()
  textbgcolor;    ///< 16-bit text color for print()
uint8_t
  textsize_x,      ///< Desired magnification in X-axis of text to print()
  textsize_y,      ///< Desired magnification in Y-axis of text to print()
  rotation;       ///< Display rotation (0 thru 3)
bool
  wrap,           ///< If set, 'wrap' text at right edge of display
  _cp437;         ///< If set, use correct CP437 charset (default is off)
GFXfont
  *gfxFont;       ///< Pointer to special font

uint8_t GFX_getRotation(void){
	return rotation;
}

void GFX_begin(int16_t w, int16_t h)
{
    _width    = w;
    _height   = h;
    rotation  = 0;
    cursor_y  = cursor_x    = 0;
    textsize_x = textsize_y  = 1;
    textcolor = textbgcolor = 0xFFFF;
    wrap      = true;
    _cp437    = false;
    gfxFont   = NULL;
}

void GFX_setTextSize(uint8_t s) {
    textsize_x = (s > 0) ? s : 1;
    textsize_y = (s > 0) ? s : 1;
}

void GFX_setTextWrap(bool w)
{
	wrap = w;
}

void GFX_setTextColor(uint16_t c)
{
	textcolor = textbgcolor = c;
}

void GFX_setCursor(int16_t x, int16_t y)
{
	cursor_x = x;
	cursor_y = y;
}

size_t GFX_print_str(const char *str)
{
	return GFX_write_str(str);
}

size_t GFX_write_str(const char *str) {
	if (str == NULL) return 0;
	return GFX_write((const uint8_t *)str, strlen(str));
}

size_t GFX_write_char(uint8_t c) {
	if(c == '\n') {                        // Newline?
		cursor_x  = 0;                     // Reset x to zero,
		cursor_y += textsize_y * 8;        // advance y one line
	} else if(c != '\r') {                 // Ignore carriage returns
		if(wrap && ((cursor_x + textsize_x * 6) > _width)) { // Off right?
			cursor_x  = 0;                 // Reset x to zero,
			cursor_y += textsize_y * 8;    // advance y one line
		}
		GFX_drawChar(cursor_x, cursor_y, c, textcolor, textbgcolor, textsize_x, textsize_y);
		cursor_x += textsize_x * 6;          // Advance x one char
	}

    return 1;
}

size_t GFX_write(const uint8_t *buffer, size_t size)
{
  size_t n = 0;
  while (size--) {
    if (GFX_write_char(*buffer++)) n++;
    else break;
  }
  return n;
}

void GFX_startWrite(){
}
void GFX_endWrite(){

}


void GFX_writePixel(int16_t x, int16_t y, uint16_t color){
	LED_drawPixel(x, y, color);
}

void GFX_writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
        uint16_t color) {

    int16_t steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) {
        _swap_int16_t(x0, y0);
        _swap_int16_t(x1, y1);
    }

    if (x0 > x1) {
        _swap_int16_t(x0, x1);
        _swap_int16_t(y0, y1);
    }

    int16_t dx, dy;
    dx = x1 - x0;
    dy = abs(y1 - y0);

    int16_t err = dx / 2;
    int16_t ystep;

    if (y0 < y1) {
        ystep = 1;
    } else {
        ystep = -1;
    }

    for (; x0<=x1; x0++) {
        if (steep) {
            GFX_writePixel(y0, x0, color);
        } else {
            GFX_writePixel(x0, y0, color);
        }
        err -= dy;
        if (err < 0) {
            y0 += ystep;
            err += dx;
        }
    }
}

void GFX_writeFastVLine(int16_t x, int16_t y,
        int16_t h, uint16_t color) {
    // Overwrite in subclasses if GFX_startWrite is defined!
    // Can be just GFX_writeLine(x, y, x, y+h-1, color);
    // or writeFillRect(x, y, 1, h, color);
    GFX_drawFastVLine(x, y, h, color);
}

void GFX_writeFastHLine(int16_t x, int16_t y,
        int16_t w, uint16_t color) {
    // Overwrite in subclasses if GFX_startWrite is defined!
    // Example: GFX_writeLine(x, y, x+w-1, y, color);
    // or writeFillRect(x, y, w, 1, color);
    GFX_drawFastHLine(x, y, w, color);
}

void GFX_writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    // Overwrite in subclasses if desired!
    GFX_fillRect(x,y,w,h,color);
}

void GFX_drawFastVLine(int16_t x, int16_t y,
        int16_t h, uint16_t color) {
    GFX_startWrite();
    GFX_writeLine(x, y, x, y+h-1, color);
    GFX_endWrite();
}

void GFX_drawFastHLine(int16_t x, int16_t y,
        int16_t w, uint16_t color) {
    GFX_startWrite();
    GFX_writeLine(x, y, x+w-1, y, color);
    GFX_endWrite();
}

void GFX_fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
        uint16_t color) {
    GFX_startWrite();
    for (int16_t i=x; i<x+w; i++) {
    	GFX_writeFastVLine(i, y, h, color);
    }
    GFX_endWrite();
}

void GFX_drawRect(int16_t x, int16_t y, int16_t w, int16_t h,
        uint16_t color) {
    GFX_startWrite();
    GFX_writeFastHLine(x, y, w, color);
    GFX_writeFastHLine(x, y+h-1, w, color);
    GFX_writeFastVLine(x, y, h, color);
    GFX_writeFastVLine(x+w-1, y, h, color);
    GFX_endWrite();
}

void GFX_drawBitmap(int16_t x, int16_t y,
  const uint8_t bitmap[], int16_t w, int16_t h, uint16_t color) {

    int16_t byteWidth = (w + 7) / 8; // Bitmap scanline pad = whole byte
    uint8_t byte = 0;

    GFX_startWrite();
    for(int16_t j=0; j<h; j++, y++) {
        for(int16_t i=0; i<w; i++) {
            if(i & 7) byte <<= 1;
            else      byte   = pgm_read_byte(&bitmap[j * byteWidth + i / 8]);
            if(byte & 0x80) GFX_writePixel(x+i, y, color);
        }
    }
    GFX_endWrite();
}

void GFX_setRotation(uint8_t x) {
    rotation = (x & 3);
    switch(rotation) {
        case 0:
        case 2:
            _width  = WIDTH;
            _height = HEIGHT;
            break;
        case 1:
        case 3:
            _width  = HEIGHT;
            _height = WIDTH;
            break;
    }
}

void GFX_drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg, uint8_t size_x, uint8_t size_y) {

	if((x >= _width)            || // Clip right
	   (y >= _height)           || // Clip bottom
	   ((x + 6 * size_x - 1) < 0) || // Clip left
	   ((y + 8 * size_y - 1) < 0))   // Clip top
		return;

	if(!_cp437 && (c >= 176)) c++; // Handle 'classic' charset behavior

	GFX_startWrite();
	for(int8_t i=0; i<5; i++ ) { // Char bitmap = 5 columns
		uint8_t line = pgm_read_byte(&font[c * 5 + i]);
		for(int8_t j=0; j<8; j++, line >>= 1) {
			if(line & 1) {
				if(size_x == 1 && size_y == 1)
					GFX_writePixel(x+i, y+j, color);
				else
					GFX_writeFillRect(x+i*size_x, y+j*size_y, size_x, size_y, color);
			} else if(bg != color) {
				if(size_x == 1 && size_y == 1)
					GFX_writePixel(x+i, y+j, bg);
				else
					GFX_writeFillRect(x+i*size_x, y+j*size_y, size_x, size_y, bg);
			}
		}
	}
	if(bg != color) { // If opaque, draw vertical line for last column
		if(size_x == 1 && size_y == 1)
			GFX_writeFastVLine(x+5, y, 8, bg);
		else
			GFX_writeFillRect(x+5*size_x, y, size_x, 8*size_y, bg);
	}
	GFX_endWrite();
}

