#ifndef _ADAFRUIT_GFX_H
#define _ADAFRUIT_GFX_H


#include "gfxfont.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define min(a,b) ((a)<(b)?(a):(b))

void GFX_setRotation(uint8_t r);
uint8_t GFX_getRotation(void);
void GFX_begin(int16_t w, int16_t h);
void GFX_setTextSize(uint8_t s);
void GFX_setTextWrap(bool w);

void GFX_setTextColor(uint16_t c);
void GFX_setCursor(int16_t x, int16_t y);

size_t GFX_print_str(const char *str);

size_t GFX_write_char(uint8_t c);
size_t GFX_write_str(const char *str);
size_t GFX_write(const uint8_t *buffer, size_t size);

extern void LED_drawPixel(int16_t x, int16_t y, uint16_t color);

void GFX_drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg, uint8_t size_x, uint8_t size_y);
void GFX_startWrite(void);
void GFX_writePixel(int16_t x, int16_t y, uint16_t color);
void GFX_writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void GFX_writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
void GFX_writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
void GFX_writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void GFX_endWrite(void);
void GFX_drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
void GFX_drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
void GFX_fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

#endif // _ADAFRUIT_GFX_H
