/*
 * Lua RTOS, ST7735 driver
 *
 * Copyright (C) 2015 - 2017
 * IBEROXARXA SERVICIOS INTEGRALES, S.L.
 *
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

/*
 * This driver is inspired and takes code from the following projects:
 *
 * Boris Lovošević, tft driver for Lua RTOS:
 *
 * https://github.com/loboris/Lua-RTOS-ESP32-lobo/tree/master/components/lua_rtos/Lua/modules/screen
 *
 * Adafruit ST7735 library:
 *
 * https://github.com/adafruit/Adafruit-ST7735-Library
 *
 */

#ifndef ST7735S_H
#define	ST7735S_H

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_GDISPLAY

#define ST7735_BUFFER 1920

// Display constants
#define INITR_GREENTAB 0x0
#define INITR_REDTAB   0x1
#define INITR_BLACKTAB 0x2

#define ST7735_NOP     0x00
#define ST7735_SWRESET 0x01
#define ST7735_RDDID   0x04
#define ST7735_RDDST   0x09

#define ST7735_SLPIN   0x10
#define ST7735_SLPOUT  0x11
#define ST7735_PTLON   0x12
#define ST7735_NORON   0x13

#define ST7735_INVOFF     0x20
#define ST7735_INVONN     0x21
#define ST7735_DISPOFF    0x28
#define ST7735_DISPON     0x29
#define ST7735_CASET      0x2A
#define ST7735_PASET      0x2B
#define ST7735_RAMWR      0x2C
#define ST7735_RAMRD      0x2E
#define ST7735_MADCTL	   0x36
#define ST7735_PTLAR 	   0x30
#define ST7735_ENTRYM 	   0xB7

#define ST7735_COLMOD  0x3A

#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_DISSET5 0xB6

#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5

#define ST7735_RDID1   0xDA
#define ST7735_RDID2   0xDB
#define ST7735_RDID3   0xDC
#define ST7735_RDID4   0xDD

#define ST7735_PWCTR6  0xFC

#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

#define ST7735_MADCTL_MY  0x80
#define ST7735_MADCTL_MX  0x40
#define ST7735_MADCTL_MV  0x20
#define ST7735_MADCTL_ML  0x10
#define ST7735_MADCTL_RGB 0x00
#define ST7735_MADCTL_BGR 0x08
#define ST7735_MADCTL_MH  0x04

#if 0
#define ILI9341_WIDTH  240
#define ILI9341_HEIGHT 320
#define XADOW_WIDTH  240
#define XADOW_HEIGHT 240

#define ILI9341_NOP     0x00
#define ILI9341_SWRESET 0x01
#define ILI9341_RDDID   0x04
#define ILI9341_RDDST   0x09

#define ILI9341_SLPIN   0x10
#define ILI9341_SLPOUT  0x11
#define ILI9341_PTLON   0x12
#define ILI9341_NORON   0x13

#define ILI9341_RDMODE  0x0A
#define ILI9341_RDMADCTL  0x0B
#define ILI9341_RDPIXFMT  0x0C
#define ILI9341_RDIMGFMT  0x0D
#define ILI9341_RDSELFDIAG  0x0F

#define ILI9341_GAMMASET 0x26

#define ILI9341_PIXFMT  0x3A

#define ILI9341_FRMCTR1 0xB1
#define ILI9341_FRMCTR2 0xB2
#define ILI9341_FRMCTR3 0xB3
#define ILI9341_INVCTR  0xB4
#define ILI9341_DFUNCTR 0xB6

#define ILI9341_PWCTR1  0xC0
#define ILI9341_PWCTR2  0xC1
#define ILI9341_PWCTR3  0xC2
#define ILI9341_PWCTR4  0xC3
#define ILI9341_PWCTR5  0xC4
#define ILI9341_VMCTR1  0xC5
#define ILI9341_VMCTR2  0xC7

#define ILI9341_RDID1   0xDA
#define ILI9341_RDID2   0xDB
#define ILI9341_RDID3   0xDC
#define ILI9341_RDID4   0xDD

#define ILI9341_GMCTRP1 0xE0
#define ILI9341_GMCTRN1 0xE1

#define ILI9341_POWERA	0xCB
#define ILI9341_POWERB	0xCF
#define ILI9341_POWER_SEQ       0xED
#define ILI9341_DTCA	0xE8
#define ILI9341_DTCB	0xEA
#define ILI9341_PRC	0xF7
#define ILI9341_3GAMMA_EN	0xF2

#endif

//  ST7735 errors
#define ST7735_CANNOT_SETUP             (DRIVER_EXCEPTION_BASE(ST7735_DRIVER_ID) |  0)
#define ST7735_ERR_NOT_ENOUGH_MEMORY    (DRIVER_EXCEPTION_BASE(ST7735_DRIVER_ID) |  1)

driver_error_t *st7735_init(uint8_t chipset, uint8_t orientation);
void st7735_set_orientation(uint8_t m);
uint32_t st7735_width();
uint32_t st7735_height();
void st7735_on();
void st7735_off();
void st7735_invert(uint8_t on);
void st7735_addr_window(uint8_t write, int x0, int y0, int x1, int y1);
void st7735_color(uint16_t *color, uint32_t len);
void st7735_clear(uint32_t color);

#endif

#endif	/* ST7735S_H */
