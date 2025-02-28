/*
 * cartridge.c - cartridge emulation
 *
 * Copyright (C) 2001-2010 Piotr Fusik
 * Copyright (C) 2001-2023 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "config.h"
#include "ff.h"

#include <stdlib.h>
#include <string.h>

#include "atari.h"
#include "binload.h" /* BINLOAD_loading_basic */
#include "cartridge.h"
#include "memory.h"
#ifdef IDE
#  include "ide.h"
#endif
#include "pia.h"
#include "rtime.h"
#include "util.h"
#ifndef BASIC
#include "statesav.h"
#endif
#ifdef AF80
#include "af80.h"
#endif
#ifdef BIT3
#include "bit3.h"
#endif
#include "log.h"

/* #define DEBUG 1 */

int CARTRIDGE_autoreboot = TRUE;

static int CartIsFor5200(int type)
{
	switch (type) {
	case CARTRIDGE_5200_32:
	case CARTRIDGE_5200_EE_16:
	case CARTRIDGE_5200_40:
	case CARTRIDGE_5200_NS_16:
	case CARTRIDGE_5200_8:
	case CARTRIDGE_5200_4:
	case CARTRIDGE_5200_SUPER_64:
	case CARTRIDGE_5200_SUPER_128:
	case CARTRIDGE_5200_SUPER_256:
	case CARTRIDGE_5200_SUPER_512:
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

static int CartIsPassthrough(int type)
{
	return type == CARTRIDGE_SDX_64 || type == CARTRIDGE_SDX_128 ||
	       type == CARTRIDGE_ATRAX_SDX_64 || type == CARTRIDGE_ATRAX_SDX_128;
}

CARTRIDGE_image_t CARTRIDGE_main = { CARTRIDGE_NONE, 0, 0, NULL, "", TRUE }; /* Left/Right cartridge */
CARTRIDGE_image_t CARTRIDGE_piggyback = { CARTRIDGE_NONE, 0, 0, NULL, "", TRUE }; /* Pass through cartridge for SpartaDOSX */

/* The currently active cartridge in the left slot - normally points to
   CARTRIDGE_main but can be switched to CARTRIDGE_piggyback if the main
   cartridge is a SpartaDOS X. */
static CARTRIDGE_image_t *active_cart = &CARTRIDGE_main;

static ULONG Calculate_RamCart_Address(int cart_type, int cart_state)
{
	int state = cart_state;
	switch (cart_type)
	{
	case CARTRIDGE_RAMCART_64:
	case CARTRIDGE_RAMCART_128:
	case CARTRIDGE_DOUBLE_RAMCART_256:
		if (!(cart_state & 0x2000)) /* 2x128K */
			state &= 0xfffb;
	case CARTRIDGE_RAMCART_1M:
		state ^= (cart_state & 0x4000) >> 12; /* exchange 128k modules */
	/*case CARTRIDGE_RAMCART_2M:
	case CARTRIDGE_RAMCART_4M:
	case CARTRIDGE_RAMCART_8M:
	case CARTRIDGE_RAMCART_16M:
	case CARTRIDGE_RAMCART_32M:*/
	}
	return ((state & 0x0038) << 11) | ((state & 0x0004) << 15) | ((state & 0x0fc0) << 12) | ((state & 0x40000) << 6);
}

static ULONG Calculate_SiDiCar_Address(int cart_type, int cart_state)
{
	return (cart_state & 0x0003) << 11;
}

/* DB_32, XEGS_32, XEGS_07_64, XEGS_128, XEGS_256, XEGS_512, XEGS_1024,
   SWXEGS_32, SWXEGS_64, SWXEGS_128, SWXEGS_256, SWXEGS_512, SWXEGS_1024 */
static void set_bank_809F(int main, int old_state)
{
	if (active_cart->state & 0x80) {
		MEMORY_Cart809fDisable();
		MEMORY_CartA0bfDisable();
	}
	else {
		MEMORY_Cart809fEnable();
		MEMORY_CartA0bfEnable();
		MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image + active_cart->state * 0x2000);
		if (old_state & 0x80)
			MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + main);
	}
}

/* XEGS_8F_64 */
static void set_bank_XEGS_8F_64(void)
{
	if (active_cart->state & 0x08)
		MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image + (active_cart->state & ~0x08) * 0x2000);
	else
		/* $8000-$9FFF is left unconnected. */
		MEMORY_dFillMem(0x8000, 0xff, 0x2000);
}

/* OSS_034M_16, OSS_043M_16, OSS_M091_16, OSS_8 */
static void set_bank_A0AF(int main, int old_state)
{
	if (active_cart->state < 0)
		MEMORY_CartA0bfDisable();
	else {
		MEMORY_CartA0bfEnable();
		if (active_cart->state == 0xff)
			/* Fill cart area with 0xFF. */
			MEMORY_dFillMem(0xa000, 0xff, 0x1000);
		else
			MEMORY_CopyFromCart(0xa000, 0xafff, active_cart->image + active_cart->state * 0x1000);
		if (old_state < 0)
			MEMORY_CopyFromCart(0xb000, 0xbfff, active_cart->image + main);
	}
}

/* WILL_64, EXP_64, DIAMOND_64, SDX_64, WILL_32, ATMAX_128, ATMAX_OLD_1024,
   ATRAX_DEC_128, ATRAX_SDX_64, TURBOSOFT_64, TURBOSOFT_128, ULTRACART_32,
   TURBO_HIT_32, THECART_128M, THECART_32M, THECART_64M, ATRAX_128, ADAWLIAH_32,
   ADAWLIAH_64, ATMAX_NEW_1024 */
static void set_bank_A0BF(int disable_mask, int bank_mask)
{
	if (active_cart->state & disable_mask)
		MEMORY_CartA0bfDisable();
	else {
		MEMORY_CartA0bfEnable();
		MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + (active_cart->state & bank_mask) * 0x2000);
	}
}

/* MEGA_16, MEGA_32, MEGA_64, MEGA_128, MEGA_256, MEGA_512, MEGA_1024,
   MEGAMAX_2048, MEGA_2048 */
static void set_bank_80BF(void)
{
	if (active_cart->state & 0x80) {
		MEMORY_Cart809fDisable();
		MEMORY_CartA0bfDisable();
	}
	else {
		MEMORY_Cart809fEnable();
		MEMORY_CartA0bfEnable();
		MEMORY_CopyFromCart(0x8000, 0xbfff, active_cart->image + (active_cart->state & 0x7f) * 0x4000);
	}
}

static void set_bank_5200_SUPER(void)
{
	MEMORY_CopyFromCart(0x4000, 0xbfff, active_cart->image + active_cart->state * 0x8000);
}

static void set_bank_SDX_128(void)
{
	if (active_cart->state & 8)
		MEMORY_CartA0bfDisable();
	else {
		MEMORY_CartA0bfEnable();
		MEMORY_CopyFromCart(0xa000, 0xbfff,
			active_cart->image + ((active_cart->state & 7) + ((active_cart->state & 0x10) >> 1)) * 0x2000);
	}
}
static void set_bank_SIC(int n)
{
	if (!(active_cart->state & 0x20))
		MEMORY_Cart809fDisable();
	else {
		MEMORY_Cart809fEnable();
		MEMORY_CopyFromCart(0x8000, 0x9fff,
			active_cart->image + (active_cart->state & n) * 0x4000);
	}
	if (active_cart->state & 0x40)
		MEMORY_CartA0bfDisable();
	else {
		MEMORY_CartA0bfEnable();
		MEMORY_CopyFromCart(0xa000, 0xbfff,
			active_cart->image + (active_cart->state & n) * 0x4000 + 0x2000);
	}
}

/* MEGA_4096 */
static void set_bank_MEGA_4096(void)
{
	if (active_cart->state == 0xff) {
		MEMORY_Cart809fDisable();
		MEMORY_CartA0bfDisable();
	}
	else {
		MEMORY_Cart809fEnable();
		MEMORY_CartA0bfEnable();
		MEMORY_CopyFromCart(0x8000, 0xbfff, active_cart->image + active_cart->state * 0x4000);
	}
}

/* Ram-Cart */
static void set_bank_RAMCART(int mask, int old_state)
{
	ULONG offset;

	if (old_state & 0x1000) {
		if (old_state & 0x0002) {
			offset = Calculate_RamCart_Address(active_cart->type, old_state & mask);
			MEMORY_CopyToCart(0x8000, 0x9fff, active_cart->image + offset);
		}
		if (old_state & 0x0001) {
			offset = Calculate_RamCart_Address(active_cart->type, old_state & mask);
			MEMORY_CopyToCart(0xa000, 0xbfff, active_cart->image + offset + 0x2000);
		}
	}

	if (active_cart->state & 0x0002) {
		offset = Calculate_RamCart_Address(active_cart->type, active_cart->state & mask);
		MEMORY_Cart809fEnable();
		if (active_cart->state & 0x1000)
			MEMORY_SetRAM(0x8000, 0x9fff);
		MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image + offset);
	}
	else
		MEMORY_Cart809fDisable();

	if ((active_cart->state & 0x0001) ^ ((active_cart->state & 0x1000) >> 12))
		MEMORY_CartA0bfDisable();
	else {
		offset = Calculate_RamCart_Address(active_cart->type, active_cart->state & mask);
		MEMORY_CartA0bfEnable();
		if (active_cart->state & 0x1000)
			MEMORY_SetRAM(0xa000, 0xbfff);
		MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + offset + 0x2000);
	}
}

/* SiDiCar */
static void set_bank_SIDICAR(int mask, int old_state)
{
	ULONG offset;

	if (old_state & 0x10) {
		offset = Calculate_SiDiCar_Address(active_cart->type, old_state & mask);
		MEMORY_CopyToCart(0x8000, 0x9fff, active_cart->image + offset);
	}

	if (active_cart->state & 0x10) {
		offset = Calculate_SiDiCar_Address(active_cart->type, active_cart->state & mask);
		MEMORY_Cart809fEnable();
		MEMORY_SetRAM(0x8000, 0x9fff);
		MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image + offset);
	}
	else
		MEMORY_Cart809fDisable();
}

/* Called on a read or write operation to page $D5. Switches banks or
   enables/disables the cartridge pointed to by *active_cart. */
static void SwitchBank(int old_state)
{
	/* All bank-switched cartridges besides two BBSB's are included in
	   this switch. The BBSB cartridges are not bank-switched by
	   access to page $D5, but in CARTRIDGE_BountyBob1() and
	   CARTRIDGE_BountyBob2(), so they need not be processed here. */
	switch (active_cart->type) {
	case CARTRIDGE_OSS_034M_16:
	case CARTRIDGE_OSS_043M_16:
		set_bank_A0AF(0x3000, old_state);
		break;
	case CARTRIDGE_OSS_M091_16:
	case CARTRIDGE_OSS_8:
		set_bank_A0AF(0x0000, old_state);
		break;
	case CARTRIDGE_WILL_64:
	case CARTRIDGE_EXP_64:
	case CARTRIDGE_DIAMOND_64:
	case CARTRIDGE_SDX_64:
	case CARTRIDGE_WILL_32:
	case CARTRIDGE_ATRAX_SDX_64:
	case CARTRIDGE_ADAWLIAH_64:
		set_bank_A0BF(8, 7);
		break;
	case CARTRIDGE_DB_32:
	case CARTRIDGE_XEGS_32:
	case CARTRIDGE_SWXEGS_32:
		set_bank_809F(0x6000, old_state);
		break;
	case CARTRIDGE_XEGS_07_64:
	case CARTRIDGE_SWXEGS_64:
		set_bank_809F(0xe000, old_state);
		break;
	case CARTRIDGE_XEGS_8F_64:
		set_bank_XEGS_8F_64();
		break;
	case CARTRIDGE_XEGS_128:
	case CARTRIDGE_SWXEGS_128:
		set_bank_809F(0x1e000, old_state);
		break;
	case CARTRIDGE_XEGS_256:
	case CARTRIDGE_SWXEGS_256:
		set_bank_809F(0x3e000, old_state);
		break;
	case CARTRIDGE_XEGS_512:
	case CARTRIDGE_SWXEGS_512:
		set_bank_809F(0x7e000, old_state);
		break;
	case CARTRIDGE_XEGS_1024:
	case CARTRIDGE_SWXEGS_1024:
		set_bank_809F(0xfe000, old_state);
		break;
	case CARTRIDGE_ATRAX_DEC_128:
	case CARTRIDGE_ATMAX_OLD_1024:
	case CARTRIDGE_ATRAX_128:
	case CARTRIDGE_ATMAX_NEW_1024:
		set_bank_A0BF(0x80, 0x7f);
		break;
	case CARTRIDGE_ATMAX_128:
	case CARTRIDGE_TURBOSOFT_64:
	case CARTRIDGE_TURBOSOFT_128:
		set_bank_A0BF(0x10, 0x0f);
		break;
	case CARTRIDGE_MEGA_16:
	case CARTRIDGE_MEGA_32:
	case CARTRIDGE_MEGA_64:
	case CARTRIDGE_MEGA_128:
	case CARTRIDGE_MEGA_256:
	case CARTRIDGE_MEGA_512:
	case CARTRIDGE_MEGA_1024:
	case CARTRIDGE_MEGA_2048:
	case CARTRIDGE_MEGAMAX_2048:
		set_bank_80BF();
		break;
	case CARTRIDGE_PHOENIX_8:
	case CARTRIDGE_BLIZZARD_4:
		if (active_cart->state)
			MEMORY_CartA0bfDisable();
		break;
	case CARTRIDGE_BLIZZARD_16:
		if (active_cart->state) {
			MEMORY_Cart809fDisable();
			MEMORY_CartA0bfDisable();
		}
		break;
	case CARTRIDGE_SDX_128:
	case CARTRIDGE_ATRAX_SDX_128:
		set_bank_SDX_128();
		break;
	case CARTRIDGE_AST_32:
		/* Value 0x10000 indicates cartridge enabled. */
		if (active_cart->state < 0x10000)
			MEMORY_CartA0bfDisable();
		break;
	case CARTRIDGE_ULTRACART_32:
	case CARTRIDGE_BLIZZARD_32:
	case CARTRIDGE_ADAWLIAH_32:
		set_bank_A0BF(4, 3);
		break;
	case CARTRIDGE_SIC_128:
		set_bank_SIC(0x07);
		break;
	case CARTRIDGE_SIC_256:
		set_bank_SIC(0x0f);
		break;
	case CARTRIDGE_SIC_512:
		set_bank_SIC(0x1f);
		break;
	case CARTRIDGE_MEGA_4096:
		set_bank_MEGA_4096();
		break;
	case CARTRIDGE_THECART_128M:
		set_bank_A0BF(0x4000, 0x3fff);
		break;
	case CARTRIDGE_THECART_32M:
		set_bank_A0BF(0x4000, 0x0fff);
		break;
	case CARTRIDGE_THECART_64M:
		set_bank_A0BF(0x4000, 0x1fff);
		break;
	case CARTRIDGE_RAMCART_64:
		set_bank_RAMCART(0x00018, old_state);
		break;
	case CARTRIDGE_RAMCART_128:
		set_bank_RAMCART(0x00038, old_state);
		break;
	case CARTRIDGE_DOUBLE_RAMCART_256:
		set_bank_RAMCART(0x0603c, old_state);
		break;
	case CARTRIDGE_RAMCART_1M:
		set_bank_RAMCART(0x000fc, old_state);
		break;
	case CARTRIDGE_RAMCART_2M:
		set_bank_RAMCART(0x001fc, old_state);
		break;
	case CARTRIDGE_RAMCART_4M:
		set_bank_RAMCART(0x003fc, old_state);
		break;
	case CARTRIDGE_RAMCART_8M:
		set_bank_RAMCART(0x007fc, old_state);
		break;
	case CARTRIDGE_RAMCART_16M:
		set_bank_RAMCART(0x00ffc, old_state);
		break;
	case CARTRIDGE_RAMCART_32M:
		set_bank_RAMCART(0x40ffc, old_state);
		break;
	case CARTRIDGE_SIDICAR_32:
		set_bank_SIDICAR(0x03, old_state);
		break;
	}
#if DEBUG
	if (old_state != active_cart->state)
		Log_print("Cart %i state: %02x -> %02x", active_cart == &CARTRIDGE_piggyback, old_state, active_cart->state);
#endif
}

void CARTRIDGE_UpdateState(CARTRIDGE_image_t *cart, int old_state)
{
	if (cart == active_cart)
		SwitchBank(old_state);
}

/* Maps *active_cart to memory. If the cartridge is bankswitched,
   the mapping is performed according to its current state (ie. it doesn't
   reset to bank 0 or whatever). */
/* Note that this function only maps part of a cartridge (if any). Then it
   calls SwitchBank(), which maps the rest. */
static void MapActiveCart(void)
{
	if (Atari800_machine_type == Atari800_MACHINE_5200) {
		MEMORY_SetROM(0x4ff6, 0x4ff9); /* disable Bounty Bob bank switching */
		MEMORY_SetROM(0x5ff6, 0x5ff9);
		MEMORY_SetROM(0xbfc0, 0xbfff); /* disable Super Cart bank switching */
		switch (active_cart->type) {
		case CARTRIDGE_5200_SUPER_64:
		case CARTRIDGE_5200_SUPER_128:
		case CARTRIDGE_5200_SUPER_256:
		case CARTRIDGE_5200_SUPER_512:
			set_bank_5200_SUPER();
#ifndef PAGED_ATTRIB
			MEMORY_SetHARDWARE(0xbfc0, 0xbfff);
#else
			MEMORY_readmap[0xbf] = CARTRIDGE_5200SuperCartGetByte;
			MEMORY_writemap[0xbf] = CARTRIDGE_5200SuperCartPutByte;
#endif
			break;
		case CARTRIDGE_5200_32:
			MEMORY_CopyFromCart(0x4000, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_5200_EE_16:
			MEMORY_CopyFromCart(0x4000, 0x5fff, active_cart->image);
			MEMORY_CopyFromCart(0x6000, 0x9fff, active_cart->image);
			MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0x2000);
			break;
		case CARTRIDGE_5200_40:
			MEMORY_CopyFromCart(0x4000, 0x4fff, active_cart->image + (active_cart->state & 0x03) * 0x1000);
			MEMORY_CopyFromCart(0x5000, 0x5fff, active_cart->image + 0x4000 + ((active_cart->state & 0x0c) >> 2) * 0x1000);
			MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image + 0x8000);
			MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0x8000);
#ifndef PAGED_ATTRIB
			MEMORY_SetHARDWARE(0x4ff6, 0x4ff9);
			MEMORY_SetHARDWARE(0x5ff6, 0x5ff9);
#else
			MEMORY_readmap[0x4f] = CARTRIDGE_BountyBob1GetByte;
			MEMORY_readmap[0x5f] = CARTRIDGE_BountyBob2GetByte;
			MEMORY_writemap[0x4f] = CARTRIDGE_BountyBob1PutByte;
			MEMORY_writemap[0x5f] = CARTRIDGE_BountyBob2PutByte;
#endif
			break;
		case CARTRIDGE_5200_NS_16:
			MEMORY_CopyFromCart(0x8000, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_5200_8:
			MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image);
			MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_5200_4:
			MEMORY_CopyFromCart(0x8000, 0x8fff, active_cart->image);
			MEMORY_CopyFromCart(0x9000, 0x9fff, active_cart->image);
			MEMORY_CopyFromCart(0xa000, 0xafff, active_cart->image);
			MEMORY_CopyFromCart(0xb000, 0xbfff, active_cart->image);
			break;
		default:
			/* clear cartridge area so the 5200 will crash */
			MEMORY_dFillMem(0x4000, 0, 0x8000);
			break;
		}
	}
	else { /* Atari800_machine_type != Atari800_MACHINE_5200 */
		switch (active_cart->type) {
		case CARTRIDGE_STD_2:
			MEMORY_Cart809fDisable();
			MEMORY_CartA0bfEnable();
			MEMORY_dFillMem(0xa000, 0xff, 0x1800);
			MEMORY_CopyFromCart(0xb800, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_STD_4:
			MEMORY_Cart809fDisable();
			MEMORY_CartA0bfEnable();
			MEMORY_dFillMem(0xa000, 0xff, 0x1000);
			MEMORY_CopyFromCart(0xb000, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_BLIZZARD_4:
			MEMORY_Cart809fDisable();
			MEMORY_CartA0bfEnable();
			MEMORY_CopyFromCart(0xa000, 0xafff, active_cart->image);
			MEMORY_CopyFromCart(0xb000, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_STD_8:
		case CARTRIDGE_PHOENIX_8:
			MEMORY_Cart809fDisable();
			MEMORY_CartA0bfEnable();
			MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_LOW_BANK_8:
			MEMORY_Cart809fEnable();
			MEMORY_CartA0bfDisable();
			MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image);
			break;
		case CARTRIDGE_STD_16:
		case CARTRIDGE_BLIZZARD_16:
			MEMORY_Cart809fEnable();
			MEMORY_CartA0bfEnable();
			MEMORY_CopyFromCart(0x8000, 0xbfff, active_cart->image);
			break;
		case CARTRIDGE_OSS_034M_16:
		case CARTRIDGE_OSS_043M_16:
			MEMORY_Cart809fDisable();
			if (active_cart->state >= 0) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xb000, 0xbfff, active_cart->image + 0x3000);
			}
			break;
		case CARTRIDGE_OSS_M091_16:
		case CARTRIDGE_OSS_8:
			MEMORY_Cart809fDisable();
			if (active_cart->state >= 0) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xb000, 0xbfff, active_cart->image);
			}
			break;
		case CARTRIDGE_WILL_64:
		case CARTRIDGE_EXP_64:
		case CARTRIDGE_DIAMOND_64:
		case CARTRIDGE_SDX_64:
		case CARTRIDGE_ATRAX_DEC_128:
		case CARTRIDGE_WILL_32:
		case CARTRIDGE_ATMAX_128:
		case CARTRIDGE_ATMAX_OLD_1024:
		case CARTRIDGE_SDX_128:
		case CARTRIDGE_ATRAX_SDX_64:
		case CARTRIDGE_ATRAX_SDX_128:
		case CARTRIDGE_TURBOSOFT_64:
		case CARTRIDGE_TURBOSOFT_128:
		case CARTRIDGE_ULTRACART_32:
		case CARTRIDGE_BLIZZARD_32:
		case CARTRIDGE_THECART_128M:
		case CARTRIDGE_THECART_32M:
		case CARTRIDGE_THECART_64M:
		case CARTRIDGE_ATRAX_128:
		case CARTRIDGE_ADAWLIAH_32:
		case CARTRIDGE_ADAWLIAH_64:
		case CARTRIDGE_ATMAX_NEW_1024:
			MEMORY_Cart809fDisable();
			break;
		case CARTRIDGE_DB_32:
		case CARTRIDGE_XEGS_32:
		case CARTRIDGE_SWXEGS_32:
			if (!(active_cart->state & 0x80)) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0x6000);
			}
			break;
		case CARTRIDGE_XEGS_07_64:
		case CARTRIDGE_SWXEGS_64:
		case CARTRIDGE_XEGS_8F_64:
			if (!(active_cart->state & 0x80)) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0xe000);
			}
			break;
		case CARTRIDGE_XEGS_128:
		case CARTRIDGE_SWXEGS_128:
			if (!(active_cart->state & 0x80)) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0x1e000);
			}
			break;
		case CARTRIDGE_XEGS_256:
		case CARTRIDGE_SWXEGS_256:
			if (!(active_cart->state & 0x80)) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0x3e000);
			}
			break;
		case CARTRIDGE_XEGS_512:
		case CARTRIDGE_SWXEGS_512:
			if (!(active_cart->state & 0x80)) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0x7e000);
			}
			break;
		case CARTRIDGE_XEGS_1024:
		case CARTRIDGE_SWXEGS_1024:
			if (!(active_cart->state & 0x80)) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0xfe000);
			}
			break;
		case CARTRIDGE_BBSB_40:
			MEMORY_Cart809fEnable();
			MEMORY_CartA0bfEnable();
			MEMORY_CopyFromCart(0x8000, 0x8fff, active_cart->image + (active_cart->state & 0x03) * 0x1000);
			MEMORY_CopyFromCart(0x9000, 0x9fff, active_cart->image + 0x4000 + ((active_cart->state & 0x0c) >> 2) * 0x1000);
			MEMORY_CopyFromCart(0xa000, 0xbfff, active_cart->image + 0x8000);
#ifndef PAGED_ATTRIB
			MEMORY_SetHARDWARE(0x8ff6, 0x8ff9);
			MEMORY_SetHARDWARE(0x9ff6, 0x9ff9);
#else
			MEMORY_readmap[0x8f] = CARTRIDGE_BountyBob1GetByte;
			MEMORY_readmap[0x9f] = CARTRIDGE_BountyBob2GetByte;
			MEMORY_writemap[0x8f] = CARTRIDGE_BountyBob1PutByte;
			MEMORY_writemap[0x9f] = CARTRIDGE_BountyBob2PutByte;
#endif
			/* No need to call SwitchBank(), return. */
			return;
		case CARTRIDGE_RIGHT_4:
			if (Atari800_machine_type == Atari800_MACHINE_800) {
				MEMORY_Cart809fEnable();
				MEMORY_dFillMem(0x8000, 0xff, 0x1000);
				MEMORY_CopyFromCart(0x9000, 0x9fff, active_cart->image);
				if ((!Atari800_disable_basic || BINLOAD_loading_basic) && MEMORY_have_basic) {
					MEMORY_CartA0bfEnable();
					MEMORY_CopyFromCart(0xa000, 0xbfff, MEMORY_basic);
				}
				else
					MEMORY_CartA0bfDisable();
			} else {
				/* there's no right slot in XL/XE */
				MEMORY_Cart809fDisable();
				MEMORY_CartA0bfDisable();
			}
			/* No need to call SwitchBank(), return. */
			return;
		case CARTRIDGE_RIGHT_8:
			if (Atari800_machine_type == Atari800_MACHINE_800) {
				MEMORY_Cart809fEnable();
				MEMORY_CopyFromCart(0x8000, 0x9fff, active_cart->image);
				if (!Atari800_builtin_basic
				    && (!Atari800_disable_basic || BINLOAD_loading_basic) && MEMORY_have_basic) {
					MEMORY_CartA0bfEnable();
					MEMORY_CopyFromCart(0xa000, 0xbfff, MEMORY_basic);
				}
				else
					MEMORY_CartA0bfDisable();
			} else {
				/* there's no right slot in XL/XE */
				MEMORY_Cart809fDisable();
				MEMORY_CartA0bfDisable();
			}
			/* No need to call SwitchBank(), return. */
			return;
		case CARTRIDGE_AST_32:
			{
				int i;
				MEMORY_Cart809fDisable();
				MEMORY_CartA0bfEnable();
				/* Copy the chosen bank 32 times over 0xa000-0xbfff. */
				for (i = 0xa000; i < 0xc000; i += 0x100)
					MEMORY_CopyFromCart(i, i + 0xff, active_cart->image + (active_cart->state & 0xffff));
			}
			break;
		case CARTRIDGE_MEGA_16:
		case CARTRIDGE_MEGA_32:
		case CARTRIDGE_MEGA_64:
		case CARTRIDGE_MEGA_128:
		case CARTRIDGE_MEGA_256:
		case CARTRIDGE_MEGA_512:
		case CARTRIDGE_MEGA_1024:
		case CARTRIDGE_MEGA_2048:
		case CARTRIDGE_MEGA_4096:
		case CARTRIDGE_SIC_128:
		case CARTRIDGE_SIC_256:
		case CARTRIDGE_SIC_512:
		case CARTRIDGE_MEGAMAX_2048:
		case CARTRIDGE_RAMCART_64:
		case CARTRIDGE_RAMCART_128:
		case CARTRIDGE_DOUBLE_RAMCART_256:
		case CARTRIDGE_RAMCART_1M:
		case CARTRIDGE_RAMCART_2M:
		case CARTRIDGE_RAMCART_4M:
		case CARTRIDGE_RAMCART_8M:
		case CARTRIDGE_RAMCART_16M:
		case CARTRIDGE_RAMCART_32M:
		case CARTRIDGE_SIDICAR_32:
			break;
		default:
			MEMORY_Cart809fDisable();
			if (!Atari800_builtin_basic
			&& (!Atari800_disable_basic || BINLOAD_loading_basic) && MEMORY_have_basic) {
				MEMORY_CartA0bfEnable();
				MEMORY_CopyFromCart(0xa000, 0xbfff, MEMORY_basic);
			}
			else
				MEMORY_CartA0bfDisable();
			/* No need to call SwitchBank(), return. */
			return;
		}
		SwitchBank(active_cart->state);
	}
}

/* Called from GetByte() and PutByte(), this function sets cartridge state
   for a cartridge that is bank-switched by either read or write to
   page $D5. Returns TRUE if switching a bank is needed. */
static int access_D5(CARTRIDGE_image_t *cart, UWORD addr, int *state)
{
	int old_state = cart->state;
	int new_state;

	switch (cart->type) {
	case CARTRIDGE_OSS_034M_16:
		/* Reference: http://www.retrobits.net/atari/osscarts.shtml
		   Deprecated by CARTRIDGE_OSS_043M_16 - 034M is an incorrect
		   bank order (a real cartridge consists of two 8KB chips,
		   one containing banks 0 and 4, the other 3 and M). Kept here
		   for backward compatibility. */
		   
		if (addr & 0x08)
			new_state = -1;
		else
			switch (addr & 0x07) {
			case 0x00:
				/* B Lo/A Hi */
				new_state = 0;
				break;
			case 0x01:
				/* A Lo+B Lo/A Hi */
				/* TODO should be binary AND of both banks. For now only fills with 0xFFs. */
				new_state = 0xff;
				break;
			case 0x03:
			case 0x07:
				/* A Lo/A Hi */
				new_state = 1;
				break;
			case 0x04:
				/* B Hi/A Hi */
				new_state = 2;
				break;
			case 0x05:
				/* A Lo+B Hi/A Hi */
				/* TODO should be binary AND of both banks. For now only fills with 0xFFs. */
				new_state = 0xff;
				break;
			default: /* 0x02, 0x06 */
				/* Fill cart area with 0xFFs. */
				new_state = 0xff;
				break;
			}
		break;
	case CARTRIDGE_OSS_043M_16:
		/* Reference: http://www.retrobits.net/atari/osscarts.shtml
		   Using the nomenclature of the above article: the emulator
		   accepts 16KB images composed of two 8KB EPROM dumps joined
		   together in the following order: ROM B, ROM A. Currently
		   only three cartridges with this scheme are known:
		   Action! 3.5, BASIC XL 1.02 and MAC/65 1.0. */
		   
		if (addr & 0x08)
			new_state = -1;
		else
			switch (addr & 0x07) {
			case 0x00:
				/* B Lo/A Hi */
				new_state = 0;
				break;
			case 0x01:
				/* A Lo+B Lo/A Hi */
				/* TODO should be binary AND of both banks. For now only fills with 0xFFs. */
				new_state = 0xff;
				break;
			case 0x03:
			case 0x07:
				/* A Lo/A Hi */
				new_state = 2;
				break;
			case 0x04:
				/* B Hi/A Hi */
				new_state = 1;
				break;
			case 0x05:
				/* A Lo+B Hi/A Hi */
				/* TODO should be binary AND of both banks. For now only fills with 0xFFs. */
				new_state = 0xff;
				break;
			default: /* 0x02, 0x06 */
				/* Fill cart area with 0xFFs. */
				new_state = 0xff;
				break;
			}
		break;
	case CARTRIDGE_DB_32:
		new_state = addr & 0x03;
		break;
	case CARTRIDGE_WILL_64:
		new_state = addr & 0x0f;
		break;
	case CARTRIDGE_WILL_32:
		new_state = addr & 0x0b;
		break;
	case CARTRIDGE_EXP_64:
		/* Only react to access to $D57x. */
		if ((addr & 0xf0) != 0x70)
			return FALSE;
		new_state = ((addr ^ 7) & 0x0f);
		break;
	case CARTRIDGE_DIAMOND_64:
		/* Only react to access to $D5Dx. */
		if ((addr & 0xf0) != 0xd0)
			return FALSE;
		new_state = ((addr ^ 7) & 0x0f);
		break;
	case CARTRIDGE_SDX_64:
	case CARTRIDGE_ATRAX_SDX_64:
		/* Only react to access to $D5Ex. */
		if ((addr & 0xf0) != 0xe0)
			return FALSE;
		if (addr & 0x08)
			new_state = addr & 0x0c;
		else
			/* Negate bits that encode bank number. */
			new_state = ((addr ^ 0x07) & 0x0f);
		if (cart == &CARTRIDGE_main) {
			/* It's the 1st cartridge, process switching the piggyback on/off. */
			if ((old_state & 0x0c) == 0x08) { /* Piggyback cartridge was enabled */
				if ((new_state & 0x0c) != 0x08)  { /* Going to disable it */
					active_cart = &CARTRIDGE_main;
					MapActiveCart();
				}
			}
			else if ((new_state & 0x0c) == 0x08) { /* Going to enable piggyback */
				active_cart = &CARTRIDGE_piggyback;
				MapActiveCart();
			}
		}
		break;
	case CARTRIDGE_SDX_128:
	case CARTRIDGE_ATRAX_SDX_128:
		/* Only react to access to $D5Ex/$D5Fx. */
		if ((addr & 0xe0) != 0xe0)
			return FALSE;
		if (addr & 0x08)
			new_state = addr & 0x0c;
		else
			/* Negate bits that encode bank number. */
			new_state = ((addr ^ 0x17) & 0x1f);
		if (cart == &CARTRIDGE_main) {
			/* It's the 1st cartridge, process switching the piggyback on/off. */
			if ((old_state & 0x0c) == 0x08) { /* Piggyback cartridge was enabled */
				if ((new_state & 0x0c) != 0x08)  { /* Going to disable it */
					active_cart = &CARTRIDGE_main;
					MapActiveCart();
				}
			}
			else if ((new_state & 0x0c) == 0x08) { /* Going to enable piggyback */
				active_cart = &CARTRIDGE_piggyback;
				MapActiveCart();
			}
		}
		break;
	case CARTRIDGE_OSS_M091_16:
		switch (addr & 0x09) {
		case 0x00:
			new_state = 1;
			break;
		case 0x01:
			new_state = 3;
			break;
		case 0x08:
			new_state = -1;
			break;
		default: /* 0x09 */
			new_state = 2;
			break;
		}
		break;
	case CARTRIDGE_BLIZZARD_4:
	case CARTRIDGE_PHOENIX_8:
	case CARTRIDGE_BLIZZARD_16:
		/* Disable the cart. */
		new_state = 1;
		break;
	case CARTRIDGE_ATMAX_128:
		/* Only react to access to $D50x/$D51x. */
		if ((addr & 0xe0) != 0)
			return FALSE;
		/* fall through */
	case CARTRIDGE_TURBOSOFT_128:
		new_state = addr & 0x1f;
		break;
	case CARTRIDGE_TURBOSOFT_64:
		new_state = addr & 0x17;
		break;
	case CARTRIDGE_ATMAX_OLD_1024:
	case CARTRIDGE_MEGAMAX_2048:
	case CARTRIDGE_ATMAX_NEW_1024:
		new_state = addr;
		break;
	case CARTRIDGE_OSS_8:
		switch (addr & 0x09) {
		case 0x00:
		case 0x01:
			new_state = 1;
			break;
		case 0x08:
			new_state = -1;
			break;
		default: /* 0x09 */
			new_state = 0;
			break;
		}
		break;
	case CARTRIDGE_ULTRACART_32:
		new_state = (old_state + 1) % 5;
		break;
	case CARTRIDGE_ADAWLIAH_32:
		new_state = (old_state + 1) & 0x03;
		break;
	case CARTRIDGE_ADAWLIAH_64:
		new_state = (old_state + 1) & 0x07;
		break;
	case CARTRIDGE_BLIZZARD_32:
		if (old_state < 4)
			new_state = old_state + 1;
		else
	default:
		/* Other cartridge types don't support enabling/disabling/banking through page D5. */
		return FALSE;
	}
	*state = new_state;
	return TRUE;
}

/* Processes bankswitching of CART when reading from a $D5xx address ADDR. */
static UBYTE GetByte(CARTRIDGE_image_t *cart, UWORD addr, int no_side_effects)
{
	int old_state = cart->state;
	int new_state = old_state;

#if DEBUG
	if (cart->type > CARTRIDGE_NONE)
		Log_print("Cart %i read: %04x", cart == &CARTRIDGE_piggyback, addr);
#endif
	/* Set the cartridge's new state. */
	/* Check types switchable by access to page D5. */
	if (!no_side_effects && access_D5(cart, addr, &new_state)) {
		/* Cartridge supports bankswitching and reacted to the given
		   ADDR. If the state changed, we need to do the bankswitch. */
		if (new_state != old_state) {
			cart->state = new_state;
			if (cart == active_cart)
				SwitchBank(old_state);
		}
	}

	/* Determine returned byte value. */
	switch (cart->type) {
	case CARTRIDGE_AST_32:
		/* cart->state contains address of current bank, therefore it
		   divides by 0x100. */
		return cart->image[(cart->state & 0xff00) | (addr & 0xff)];
	case CARTRIDGE_SIC_512:
	case CARTRIDGE_SIC_256:
	case CARTRIDGE_SIC_128:
	case CARTRIDGE_MEGA_4096:
		/* Only react to access to $D50x/$D51x. */
		if ((addr & 0xe0) == 0x00)
			return cart->state;
		break;
	case CARTRIDGE_THECART_128M:
	case CARTRIDGE_THECART_32M:
	case CARTRIDGE_THECART_64M:
		switch (addr) {
		case 0xd5a0:
			return cart->state & 0x00ff;
		case 0xd5a1:
			return (cart->state & 0x3f00) >> 8;
		case 0xd5a2:
			return (~cart->state & 0x4000) >> 14;
		}
		break;
	case CARTRIDGE_RAMCART_32M:
	case CARTRIDGE_RAMCART_16M:
	case CARTRIDGE_RAMCART_8M:
		if (addr == 0xd501)
			return ((cart->state & 0x0f00) >> 8) | ((cart->state & 0x40000) >> 14);
		else
	case CARTRIDGE_RAMCART_4M:
	case CARTRIDGE_RAMCART_2M:
		if (addr == 0xd500)
			return cart->state;
		break;
	case CARTRIDGE_RAMCART_1M:
		if ((!(cart->state & 0x10000) || (addr == 0xd500)) && (cart->state & 0x20000))
			return cart->state;
		break;
	case CARTRIDGE_DOUBLE_RAMCART_256:
		if (cart->state & 0x20000)
			return cart->state | 0x00c0;
	/*case CARTRIDGE_RAMCART_128:
	case CARTRIDGE_RAMCART_64:
		return cart->state | 0x00e0;*/
	}
	return 0xff;
}

/* Processes bankswitching of CART when writing to a $D5xx address ADDR. */
static void PutByte(CARTRIDGE_image_t *cart, UWORD addr, UBYTE byte)
{
	int old_state = cart->state;
	int new_state = old_state;

#if DEBUG
	if (cart->type > CARTRIDGE_NONE)
		Log_print("Cart %i write: %04x, %02x", cart == &CARTRIDGE_piggyback, addr, byte);
#endif
	/* Set the cartridge's new state. */
	switch (cart->type) {
	case CARTRIDGE_XEGS_32:
		new_state = byte & 0x03;
		break;
	case CARTRIDGE_XEGS_07_64:
		new_state = byte & 0x07;
		break;
	case CARTRIDGE_XEGS_128:
	case CARTRIDGE_XEGS_8F_64:
		new_state = byte & 0x0f;
		break;
	case CARTRIDGE_XEGS_256:
		new_state = byte & 0x1f;
		break;
	case CARTRIDGE_XEGS_512:
		new_state = byte & 0x3f;
		break;
	case CARTRIDGE_XEGS_1024:
		new_state = byte & 0x7f;
		break;
	case CARTRIDGE_MEGA_16:
		new_state = byte & 0x80;
		break;
	case CARTRIDGE_MEGA_32:
		new_state = byte & 0x81;
		break;
	case CARTRIDGE_MEGA_64:
	case CARTRIDGE_SWXEGS_32:
		new_state = byte & 0x83;
		break;
	case CARTRIDGE_MEGA_128:
	case CARTRIDGE_SWXEGS_64:
		new_state = byte & 0x87;
		break;
	case CARTRIDGE_MEGA_256:
	case CARTRIDGE_SWXEGS_128:
	case CARTRIDGE_ATRAX_DEC_128:
	case CARTRIDGE_ATRAX_128:
		new_state = byte & 0x8f;
		break;
	case CARTRIDGE_MEGA_512:
	case CARTRIDGE_SWXEGS_256:
		new_state = byte & 0x9f;
		break;
	case CARTRIDGE_MEGA_1024:
	case CARTRIDGE_SWXEGS_512:
		new_state = byte & 0xbf;
		break;
	case CARTRIDGE_MEGA_2048:
	case CARTRIDGE_SWXEGS_1024:
		new_state = byte;
		break;
	case CARTRIDGE_AST_32:
		/* State contains address of current bank. */
		new_state = (old_state + 0x100) & 0x7fff;
		break;
	case CARTRIDGE_SIC_512:
	case CARTRIDGE_SIC_256:
	case CARTRIDGE_SIC_128:
	case CARTRIDGE_MEGA_4096:
		/* Only react to access to $D50x/$D51x. */
		if ((addr & 0xe0) == 0x00)
			new_state = byte;
		break;
	case CARTRIDGE_THECART_128M:
	case CARTRIDGE_THECART_32M:
	case CARTRIDGE_THECART_64M:
		switch (addr) {
		case 0xd5a0:
			new_state = (old_state & 0x3f00) | byte;
			break;
		case 0xd5a1:
			new_state = (old_state & 0x00ff) | ((byte & 0x3f) << 8);
			break;
		case 0xd5a2:
			new_state = (old_state & 0x3fff) | ((~byte & 0x01) << 14);
			break;
		}
		break;
	case CARTRIDGE_RAMCART_64:
		if (!(old_state & 0x0004)) /* lock bit not set */
			new_state = (old_state & 0x7f000) | (byte & 0x1f);
		break;
	case CARTRIDGE_RAMCART_128:
		if (!(old_state & 0x0004)) /* lock bit not set */
			new_state = (old_state & 0x7f000) | (byte & 0x3f);
		break;
	case CARTRIDGE_DOUBLE_RAMCART_256:
		if ((old_state & 0x2000) || !(old_state & 0x0004)) /* 256K mode or lock bit not set */
			new_state = (old_state & 0x7f000) | (byte & 0x3f);
		break;
	case CARTRIDGE_RAMCART_1M:
		if (!(cart->state & 0x10000) || (addr == 0xd500)) {
			if (old_state & 0x8000) /* modified version (jumpers ABC installed) */
				new_state = (old_state & 0x7ff00) | byte;
			else /* stock version */
				new_state = (old_state & 0x7ffc4) | (byte & 0x3b);
		}
		break;
	case CARTRIDGE_RAMCART_2M:
	case CARTRIDGE_RAMCART_4M:
		if (addr == 0xd500) {
			if (old_state & 0x8000) /* modified version (jumpers ABC installed) */
				new_state = (old_state & 0x7ff00) | byte;
			else /* stock version */
				new_state = (old_state & 0x7ffc4) | (byte & 0x3b);
		}
		break;
	case CARTRIDGE_RAMCART_8M:
		if (addr == 0xd501)
			new_state = (old_state & 0x3f0ff) | ((byte & 0x07) << 8);
		else
	case CARTRIDGE_RAMCART_16M:
		if (addr == 0xd501)
			new_state = (old_state & 0x3f0ff) | ((byte & 0x0f) << 8);
		else
	case CARTRIDGE_RAMCART_32M:
		if (addr == 0xd501)
			new_state = (old_state & 0x3f0ff) | ((byte & 0x0f) << 8) | ((byte & 0x10) << 14);
		else if (addr == 0xd500)
			new_state = (old_state & 0x7ff00) | byte;
		break;
	case CARTRIDGE_SIDICAR_32:
		/* Although the $D5FF location is proposed by the author for the CARSET control register,
		   a simplified decoder built based on the use of the /CCTL signal is used. */
		if (/*(addr == 0xd5ff) &&*/ !(byte & 0x80))
			new_state = byte & 0x13;
		break;
	default:
		/* Check types switchable by access to page D5. */
		if (!access_D5(cart, addr, &new_state))
			/* Cartridge doesn't support bankswitching, or didn't react to
			   the given ADDR. */
			return;
	}

	/* If the state changed, we need to do the bankswitch. */
	if (new_state != old_state) {
		cart->state = new_state;
		if (cart == active_cart)
			SwitchBank(old_state);
	}
}

/* a read from D500-D5FF area */
UBYTE CARTRIDGE_GetByte(UWORD addr, int no_side_effects)
{
#ifdef AF80
	if (AF80_enabled) {
		return AF80_D5GetByte(addr, no_side_effects);
	}
#endif
#ifdef BIT3
	if (BIT3_enabled) {
		return BIT3_D5GetByte(addr, no_side_effects);
	}
#endif
	if (RTIME_enabled && (addr == 0xd5b8 || addr == 0xd5b9))
		return RTIME_GetByte();
#ifdef IDE
	if (IDE_enabled && (addr <= 0xd50f))
		return IDE_GetByte(addr, no_side_effects);
#endif
	/* In case 2 cartridges are inserted, reading a memory location would
	   result in binary AND of both cartridges. */
	return GetByte(&CARTRIDGE_main, addr, no_side_effects) & GetByte(&CARTRIDGE_piggyback, addr, no_side_effects);
}

/* a write to D500-D5FF area */
void CARTRIDGE_PutByte(UWORD addr, UBYTE byte)
{
#ifdef AF80
	if (AF80_enabled) {
		AF80_D5PutByte(addr,byte);
		/* Return, because AF_80_enabled means there's an AF80
		   cartridge in the left slot and no other cartridges are
		   there. */
		return;
	}
#endif
#ifdef BIT3
	if (BIT3_enabled && (addr == 0xd508 || addr == 0xd580 || addr == 0xd581 || addr == 0xd583 || addr == 0xd585)) {
		BIT3_D5PutByte(addr,byte);
	}
#endif
	if (RTIME_enabled && (addr == 0xd5b8 || addr == 0xd5b9)) {
		RTIME_PutByte(byte);
	}
#ifdef IDE
	if (IDE_enabled && (addr <= 0xd50f)) {
		IDE_PutByte(addr,byte);
	}
#endif
	PutByte(&CARTRIDGE_main, addr, byte);
	PutByte(&CARTRIDGE_piggyback, addr, byte);
}

/* special support of Atari 5200 and Atari 8-bit Bounty Bob */

/* addr must be $4fxx in 5200 mode or $8fxx in 800 mode. */
static void access_BountyBob1(UWORD addr)
{
	UWORD base_addr = (addr & 0xf000);
	addr &= 0x00ff;
	if (addr >= 0xf6 && addr <= 0xf9) {
		int new_state;
		addr -= 0xf6;
		new_state = (active_cart->state & 0x0c) | addr;
		if (new_state != active_cart->state) {
			MEMORY_CopyFromCart(base_addr, base_addr + 0x0fff,
			               active_cart->image + addr * 0x1000);
			active_cart->state = new_state;
		}
	}
}

/* addr must be $5fxx in 5200 mode or $9fxx in 800 mode. */
static void access_BountyBob2(UWORD addr)
{
	UWORD base_addr = (addr & 0xf000);
	addr &= 0x00ff;
	if (addr >= 0xf6 && addr <= 0xf9) {
		int new_state;
		addr -= 0xf6;
		new_state = (active_cart->state & 0x03) | (addr << 2);
		if (new_state != active_cart->state) {
			MEMORY_CopyFromCart(base_addr, base_addr + 0x0fff,
			               active_cart->image + 0x4000 + addr * 0x1000);
			active_cart->state = new_state;
		}
	}
}

/* addr must be $bfxx in 5200 mode only. */
static void access_5200SuperCart(UWORD addr)
{
	int old_state = active_cart->state;
	int new_state = old_state;

	if ((addr & 0xc0) == 0xc0) {
		switch (addr & 0x30) {
		case 0x00: /* $BFCx */
			new_state = (new_state & 0x03) | (addr & 0x0c);
			break;
		case 0x10: /* $BFDx */
			new_state = (new_state & 0x0c) | ((addr & 0x0c) >> 2);
			break;
		default: /* 0x20 or 0x30, i.e. $BFEx or $BFFx */
			new_state = 0x0f;
			break;
		}
		new_state &= ((active_cart->size >> 5) - 1);
	}

	if (old_state != new_state) {
		active_cart->state = new_state;
		set_bank_5200_SUPER();
	}
}

UBYTE CARTRIDGE_BountyBob1GetByte(UWORD addr, int no_side_effects)
{
	if (!no_side_effects)
		access_BountyBob1(addr);
	return MEMORY_dGetByte(addr);
}

UBYTE CARTRIDGE_BountyBob2GetByte(UWORD addr, int no_side_effects)
{
	if (!no_side_effects)
		access_BountyBob2(addr);
	return MEMORY_dGetByte(addr);
}

UBYTE CARTRIDGE_5200SuperCartGetByte(UWORD addr, int no_side_effects)
{
	if (!no_side_effects)
		access_5200SuperCart(addr);
	return MEMORY_dGetByte(addr);
}

void CARTRIDGE_BountyBob1PutByte(UWORD addr, UBYTE value)
{
	access_BountyBob1(addr);
}

void CARTRIDGE_BountyBob2PutByte(UWORD addr, UBYTE value)
{
	access_BountyBob2(addr);
}

void CARTRIDGE_5200SuperCartPutByte(UWORD addr, UBYTE value)
{
	access_5200SuperCart(addr);
}

static void ResetCartState(CARTRIDGE_image_t *cart)
{
	switch (cart->type) {
	case CARTRIDGE_OSS_034M_16:
		cart->state = 1;
		break;
	case CARTRIDGE_ATMAX_OLD_1024:
		cart->state = 0x7f;
		break;
	case CARTRIDGE_AST_32:
		/* A special value of 0x10000 indicates the cartridge is
		   enabled and the current bank is 0. */
		cart->state = 0x10000;
		break;
	case CARTRIDGE_MEGA_4096:
		cart->state = 254;
		break;
	default:
		cart->state = 0;
	}
}

/* Before first use of the cartridge, preprocess its contents if needed. */
static void PreprocessCart(CARTRIDGE_image_t *cart)
{
	/* On Atrax game and SDX cartridges, address and data lines between the
	   cartridge port and the EPROM are intermixed as a kind of copy
	   prevention. Data in the ROM chip, if read directly, would make no
	   sense. We have to decode the ROM contents according to the connections
	   on the cartidge. */

	struct cross_map_t {
		unsigned int addr[17]; /* Mapping of address (+ bank select) lines */
		unsigned char data[8]; /* Mapping of data lines */
	};
	static struct cross_map_t cross_maps[2] = {
		/* Atrax games cartridge */
		{ /* cartridge port + bank select <-> EPROM */
			{ 0x0020,              /*  A0 <->  A5 */
			  0x0040,              /*  A1 <->  A6 */
			  0x0080,              /*  A2 <->  A7 */
			  0x1000,              /*  A3 <-> A12 */
			  0x0001,              /*  A4 <->  A0 */
			  0x0002,              /*  A5 <->  A1 */
			  0x0004,              /*  A6 <->  A2 */
			  0x0008,              /*  A7 <->  A3 */
			  0x0010,              /*  A8 <->  A4 */
			  0x0100,              /*  A9 <->  A8 */
			  0x0400,              /* A10 <-> A10 */
			  0x0800,              /* A11 <-> A11 */
			  0x0200,              /* A12 <->  A9 */
			  0x2000,              /* A13 <-> A13 */
			  0x4000,              /* A14 <-> A14 */
			  0x8000,              /* A15 <-> A15 */
			  0x10000 },           /* A16 <-> A16 */
			{ 0x10,                /*  D4 <->  Q0 */
			  0x20,                /*  D5 <->  Q1 */
			  0x04,                /*  D2 <->  Q2 */
			  0x80,                /*  D7 <->  Q3 */
			  0x08,                /*  D3 <->  Q4 */
			  0x01,                /*  D0 <->  Q5 */
			  0x02,                /*  D1 <->  Q6 */
			  0x40 }               /*  D6 <->  Q7 */
		},
		/* Atrax SDX */
		{ /* cartridge port + bank select <-> EPROM */
			{ 0x0040,              /*  A0 <->  A6 */
			  0x0080,              /*  A1 <->  A7 */
			  0x1000,              /*  A2 <-> A12 */
			  0x8000,              /*  A3 <-> A15 */
			  0x4000,              /*  A4 <-> A14 */
			  0x2000,              /*  A5 <-> A13 */
			  0x0100,              /*  A6 <->  A8 */
			  0x0020,              /*  A7 <->  A5 */
			  0x0010,              /*  A8 <->  A4 */
			  0x0008,              /*  A9 <->  A3 */
			  0x0001,              /* A10 <->  A0 */
			  0x0002,              /* A11 <->  A1 */
			  0x0004,              /* A12 <->  A2 */
			  0x0200,              /* A13 <->  A9 */
			  0x0800,              /* A14 <-> A11 */
			  0x0400,              /* A15 <-> A10 */
			  0x10000 },           /* A16 <-> A16 (only on ATRAX_SDX_128) */
			{ 0x02,                /*  D1 <->  Q0 */
			  0x08,                /*  D3 <->  Q1 */
			  0x80,                /*  D7 <->  Q2 */
			  0x40,                /*  D6 <->  Q3 */
			  0x01,                /*  D0 <->  Q4 */
			  0x04,                /*  D2 <->  Q5 */
			  0x20,                /*  D5 <->  Q6 */
			  0x10 }               /*  D4 <->  Q7 */
		}
	};
	struct cross_map_t *map;

	switch (cart->type) {
	case CARTRIDGE_ATRAX_128:
		map = &cross_maps[0];
		break;
	case CARTRIDGE_ATRAX_SDX_64:
	case CARTRIDGE_ATRAX_SDX_128:
		map = &cross_maps[1];
		break;
	default: /* Cartridge lined not crossed */
		return;
	}

	{
		unsigned int i;
		unsigned int const size = cart->size << 10;
		UBYTE *new_image = (UBYTE *) Util_malloc(size, "PreprocessCart new_image");
		/* FIXME: Can be optimised by caching the results in a conversion
		   table, but doesn't seem to be worth it. */
		for (i = 0; i < size; i++) {
			unsigned int const rom_addr =
				(i &  0x0001 ? map->addr[0] : 0) |
				(i &  0x0002 ? map->addr[1] : 0) |
				(i &  0x0004 ? map->addr[2] : 0) |
				(i &  0x0008 ? map->addr[3] : 0) |
				(i &  0x0010 ? map->addr[4] : 0) |
				(i &  0x0020 ? map->addr[5] : 0) |
				(i &  0x0040 ? map->addr[6] : 0) |
				(i &  0x0080 ? map->addr[7] : 0) |
				(i &  0x0100 ? map->addr[8] : 0) |
				(i &  0x0200 ? map->addr[9] : 0) |
				(i &  0x0400 ? map->addr[10] : 0) |
				(i &  0x0800 ? map->addr[11] : 0) |
				(i &  0x1000 ? map->addr[12] : 0) |
				(i &  0x2000 ? map->addr[13] : 0) |
				(i &  0x4000 ? map->addr[14] : 0) |
				(i &  0x8000 ? map->addr[15] : 0) |
				(i & 0x10000 ? map->addr[16] : 0);

			UBYTE byte = cart->image[rom_addr];
			new_image[i] =
				(byte & 0x01 ? map->data[0] : 0) |
				(byte & 0x02 ? map->data[1] : 0) |
				(byte & 0x04 ? map->data[2] : 0) |
				(byte & 0x08 ? map->data[3] : 0) |
				(byte & 0x10 ? map->data[4] : 0) |
				(byte & 0x20 ? map->data[5] : 0) |
				(byte & 0x40 ? map->data[6] : 0) |
				(byte & 0x80 ? map->data[7] : 0);
		}
		free(cart->image);
		cart->image = new_image;
	}
}

/* Initialises the cartridge CART after mounting. Called by CARTRIDGE_Insert,
   or CARTRIDGE_Insert_Second and CARTRIDGE_SetType. */
static void InitCartridge(CARTRIDGE_image_t *cart)
{
	PreprocessCart(cart);
	ResetCartState(cart);
	if (cart == &CARTRIDGE_main) {
		/* Check if we should automatically switch between computer/5200. */
		int for5200 = CartIsFor5200(CARTRIDGE_main.type);
		if (for5200 && Atari800_machine_type != Atari800_MACHINE_5200) {
			Atari800_SetMachineType(Atari800_MACHINE_5200);
			MEMORY_ram_size = 16;
			Atari800_InitialiseMachine();
		}
		else if (!for5200 && Atari800_machine_type == Atari800_MACHINE_5200) {
			Atari800_SetMachineType(Atari800_MACHINE_XLXE);
			MEMORY_ram_size = 64;
			Atari800_InitialiseMachine();
		}
	}
	if (cart == active_cart)
		MapActiveCart();
}

int CARTRIDGE_WriteImage(char *filename, int type, UBYTE *image, int size, int raw, UBYTE value) {
	FIL f;
	FIL *fp = &f;
	FRESULT fr = f_open(fp, filename, FA_WRITE | FA_CREATE_ALWAYS);
	if (fp != NULL) {
		if (!raw) {
			UBYTE header[0x10];
			int checksum = 0;
			if (image != NULL)
				checksum = CARTRIDGE_Checksum(image, size);

			header[0x0] = 'C';
			header[0x1] = 'A';
			header[0x2] = 'R';
			header[0x3] = 'T';

			header[0x4] = 0;
			header[0x5] = 0;
			header[0x6] = 0;
			header[0x7] = type;
			
			header[0x8] = checksum >> 24;
			header[0x9] = checksum >> 16;
			header[0xa] = checksum >> 8;
			header[0xb] = checksum;
			
			header[0xc] = 0;
			header[0xd] = 0;
			header[0xe] = 0;
			header[0xf] = 0;

			fwrite(&header, 1, sizeof(header), fp);
		}
		if (image != NULL)
			fwrite(image, 1, size, fp);
		else
			while (size-- > 0)
				fwrite(&value, 1, 1, fp);

		fclose(fp);
		return 0;
	}
	else {
		Log_print("Error writing cartridge \"%s\".\n", filename);
		return -1;
	}
}

static void RemoveCart(CARTRIDGE_image_t *cart)
{
	if (cart->image != NULL) {
		switch (cart->type) {
		case CARTRIDGE_RAMCART_64:
		case CARTRIDGE_RAMCART_128:
		case CARTRIDGE_DOUBLE_RAMCART_256:
		case CARTRIDGE_RAMCART_1M:
		case CARTRIDGE_RAMCART_2M:
		case CARTRIDGE_RAMCART_4M:
		case CARTRIDGE_RAMCART_8M:
		case CARTRIDGE_RAMCART_16M:
		case CARTRIDGE_RAMCART_32M:
		case CARTRIDGE_SIDICAR_32:
			CARTRIDGE_WriteImage(cart->filename, cart->type, cart->image, cart->size << 10, cart->raw, -1);
		}

		free(cart->image);
		cart->image = NULL;
	}
	if (cart->type != CARTRIDGE_NONE) {
		cart->type = CARTRIDGE_NONE;
		if (cart == active_cart)
			MapActiveCart();
	}
}

/* Called after inserting/removing a cartridge (but not the piggyback one).
   If needed, reboots the machine. */
static void AutoReboot(void)
{
	if (CARTRIDGE_autoreboot)
		Atari800_Coldstart();
}

void CARTRIDGE_SetType(CARTRIDGE_image_t *cart, int type)
{
	cart->type = type;
	if (type == CARTRIDGE_NONE)
		/* User cancelled setting the cartridge's type - the cartridge
		   can be unloaded. */
		RemoveCart(cart);
	InitCartridge(cart);
}

void CARTRIDGE_SetTypeAutoReboot(CARTRIDGE_image_t *cart, int type)
{
	CARTRIDGE_SetType(cart, type);
	/* We don't want to autoreboot on inserting the piggyback cartridge. */
	if (cart != &CARTRIDGE_piggyback)
		AutoReboot();
}

void CARTRIDGE_ColdStart(void) {
	active_cart = &CARTRIDGE_main;
	ResetCartState(&CARTRIDGE_main);
	ResetCartState(&CARTRIDGE_piggyback);
	MapActiveCart();
}

int CARTRIDGE_ReadImage(const char *filename, CARTRIDGE_image_t *cart)
{
	FIL f;
	FIL *fp = &f;
	int len;
	int type;
	UBYTE header[16];

	/* open file */
	if (FR_OK != f_open(&f, filename, FA_READ))
		return CARTRIDGE_CANT_OPEN;
	/* check file length */
	len = Util_flen(fp);
	Util_rewind(fp);

	/* Guard against providing cart->filename as parameter. */
	if (cart->filename != filename)
		/* Save Filename for state save */
		strcpy(cart->filename, filename);

	cart->raw = TRUE;

	/* if full kilobytes, assume it is raw image */
	if ((len & 0x3ff) == 0) {
		/* alloc memory and read data */
		cart->image = (UBYTE *) Util_malloc(len, "CARTRIDGE_ReadImage cart");
		if (fread(cart->image, 1, len, fp) < len) {
			Log_print("Error reading cartridge.\n");
			fclose(fp);
			free(cart->image);
			cart->image = NULL;
			return CARTRIDGE_TOO_FEW_DATA;
		}
		fclose(fp);
		/* find cart type */
		cart->type = CARTRIDGE_NONE;
		len >>= 10;	/* number of kilobytes */
		cart->size = len;
		for (type = 1; type < CARTRIDGE_TYPE_COUNT; type++)
			if (CARTRIDGES[type].kb == len) {
				if (cart->type == CARTRIDGE_NONE) {
					cart->type = type;
				} else {
					/* more than one cartridge type of such length - user must select */
					cart->type = CARTRIDGE_UNKNOWN;
					return len;
				}
			}
		if (cart->type != CARTRIDGE_NONE) {
			/*InitCartridge(cart);*/
			return 0;	/* ok */
		}
		free(cart->image);
		cart->image = NULL;
		return CARTRIDGE_BAD_FORMAT;
	}

	/* if not full kilobytes, assume it is CART file */
	if (fread(header, 1, 16, fp) < 16) {
		Log_print("Error reading cartridge.\n");
		fclose(fp);
		return CARTRIDGE_BAD_FORMAT;
	}
	if ((header[0] == 'C') &&
		(header[1] == 'A') &&
		(header[2] == 'R') &&
		(header[3] == 'T')) {
		type = (header[4] << 24) |
			(header[5] << 16) |
			(header[6] << 8) |
			header[7];
		if (type >= 1 && type < CARTRIDGE_TYPE_COUNT) {
			int checksum;
			int result;
			len = CARTRIDGES[type].kb << 10;
			cart->raw = FALSE;
			cart->size = CARTRIDGES[type].kb;
			/* alloc memory and read data */
			cart->image = (UBYTE *) Util_malloc(len, "CARTRIDGE_ReadImage cart->image");
			if (fread(cart->image, 1, len, fp) < len) {
				Log_print("Error reading cartridge.\n");
				fclose(fp);
				free(cart->image);
				cart->image = NULL;
				return CARTRIDGE_TOO_FEW_DATA;
			}
			fclose(fp);
			checksum = (header[8] << 24) |
				(header[9] << 16) |
				(header[10] << 8) |
				header[11];
			cart->type = type;
			result = checksum == CARTRIDGE_Checksum(cart->image, len) ? 0 : CARTRIDGE_BAD_CHECKSUM;
			/*InitCartridge(cart);*/
			return result;
		}
	}
	fclose(fp);
	return CARTRIDGE_BAD_FORMAT;
}

/* Loads a cartridge from FILENAME. Copies FILENAME to CART->FILENAME.
   If loading failed, sets CART->TYPE to CARTRIDGE_NONE and returns one of:
   * CARTRIDGE_CANT_OPEN if there was an error when opening file,
   * CARTRIDGE_BAD_FORMAT if the file is not a proper cartridge image.

   If loading succeeded, allocates a buffer with cartridge image data and puts
   it in CART->IMAGE. Then sets CART->TYPE if possible, and returns one of:
   * 0 if cartridge type was recognized; CART->TYPE is then set correctly;
   * CARTRIDGE_BAD_CHECKSUM if cartridge is a CART file but with invalid
     checksum; CART->TYPE is then set correctly;
   * a positive integer: size in KB if cartridge type was not guessed;
     CART->TYPE is then set to CARTRIDGE_UNKNOWN. The caller is expected to
     select a cartridge type according to the returned size, and call either
     CARTRIDGE_SetType() or CARTRIDGE_SetTypeAutoReboot(). */
static int InsertCartridge(const char *filename, CARTRIDGE_image_t *cart)
{
	int kb = CARTRIDGE_ReadImage(filename, cart);
	if ((kb == CARTRIDGE_BAD_CHECKSUM) || (kb == 0)) {
		InitCartridge(cart);
	}
	return kb;
}

int CARTRIDGE_Insert(const char *filename)
{
	/* remove currently inserted cart */
	CARTRIDGE_Remove();
	return InsertCartridge(filename, &CARTRIDGE_main);
}

int CARTRIDGE_InsertAutoReboot(const char *filename)
{
	int result = CARTRIDGE_Insert(filename);
	AutoReboot();
	return result;
}

int CARTRIDGE_Insert_Second(const char *filename)
{
	/* remove currently inserted cart */
	CARTRIDGE_Remove_Second();
	return InsertCartridge(filename, &CARTRIDGE_piggyback);
}

void CARTRIDGE_Remove(void)
{
	active_cart = &CARTRIDGE_main;
	CARTRIDGE_Remove_Second();
	RemoveCart(&CARTRIDGE_main);
}

void CARTRIDGE_RemoveAutoReboot(void)
{
	CARTRIDGE_Remove();
	AutoReboot();
}

void CARTRIDGE_Remove_Second(void)
{
	RemoveCart(&CARTRIDGE_piggyback);
}

int CARTRIDGE_ReadConfig(char *string, char *ptr)
{
	if (strcmp(string, "CARTRIDGE_FILENAME") == 0) {
		Util_strlcpy(CARTRIDGE_main.filename, ptr, sizeof(CARTRIDGE_main.filename));
		if (CARTRIDGE_main.type == CARTRIDGE_NONE)
			CARTRIDGE_main.type = CARTRIDGE_UNKNOWN;
	}
	else if (strcmp(string, "CARTRIDGE_TYPE") == 0) {
		int value = Util_sscandec(ptr);
		if (value < 0 || value >= CARTRIDGE_TYPE_COUNT)
			return FALSE;
		CARTRIDGE_main.type = value;
	}
	else if (strcmp(string, "CARTRIDGE_PIGGYBACK_FILENAME") == 0) {
		Util_strlcpy(CARTRIDGE_piggyback.filename, ptr, sizeof(CARTRIDGE_piggyback.filename));
		if (CARTRIDGE_piggyback.type == CARTRIDGE_NONE)
			CARTRIDGE_piggyback.type = CARTRIDGE_UNKNOWN;
	}
	else if (strcmp(string, "CARTRIDGE_PIGGYBACK_TYPE") == 0) {
		int value = Util_sscandec(ptr);
		if (value < 0 || value >= CARTRIDGE_TYPE_COUNT)
			return FALSE;
		CARTRIDGE_piggyback.type = value;
	}
	else if (strcmp(string, "CARTRIDGE_AUTOREBOOT") == 0) {
		int value = Util_sscanbool(ptr);
		if (value < 0)
			return FALSE;
		CARTRIDGE_autoreboot = value;
	}
	else return FALSE;
	return TRUE;
}

void CARTRIDGE_WriteConfig(FIL *fp)
{
	fprintf(fp, "CARTRIDGE_FILENAME=%s\n", CARTRIDGE_main.filename);
	fprintf(fp, "CARTRIDGE_TYPE=%d\n", CARTRIDGE_main.type);
	fprintf(fp, "CARTRIDGE_PIGGYBACK_FILENAME=%s\n", CARTRIDGE_piggyback.filename);
	fprintf(fp, "CARTRIDGE_PIGGYBACK_TYPE=%d\n", CARTRIDGE_piggyback.type);
	fprintf(fp, "CARTRIDGE_AUTOREBOOT=%d\n", CARTRIDGE_autoreboot);
}

static void InitInsert(CARTRIDGE_image_t *cart)
{
	if (cart->type != CARTRIDGE_NONE) {
		int tmp_type = cart->type;
		int res = InsertCartridge(cart->filename, cart);
		if (res < 0) {
			Log_print("Error inserting cartridge \"%s\": %s", cart->filename,
			res == CARTRIDGE_CANT_OPEN ? "Can't open file" :
			res == CARTRIDGE_BAD_FORMAT ? "Bad format" :
			/* Assume r == CARTRIDGE_BAD_CHECKSUM */ "Bad checksum");
			cart->type = CARTRIDGE_NONE;
		}
		if (cart->type == CARTRIDGE_UNKNOWN && CARTRIDGES[tmp_type].kb == res)
			CARTRIDGE_SetType(cart, tmp_type);
	}
}

int CARTRIDGE_Initialise(int *argc, char *argv[])
{
	printf("CARTRIDGE_Initialise");
	int i;
	int j;
	int help_only = FALSE;
	/* When filename is given at commandline, we have to reset cartridge type to UNKNOWN,
	   because the cartridge type read earlier from the config file is no longer valid.
	   These two variables indicate that cartridge type is also given at commandline
	   and so it shouldn't be reset. */
	int type_from_commandline = FALSE;
	int type2_from_commandline = FALSE;

	for (i = j = 1; i < *argc; i++) {
		int i_a = (i + 1 < *argc); /* is argument available? */
		int a_m = FALSE; /* error, argument missing! */
		int a_i = FALSE; /* error, argument invalid! */
		
		if (strcmp(argv[i], "-cart") == 0) {
			if (i_a) {
				Util_strlcpy(CARTRIDGE_main.filename, argv[++i], sizeof(CARTRIDGE_main.filename));
				if (!type_from_commandline)
					CARTRIDGE_main.type = CARTRIDGE_UNKNOWN;
			}
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-cart-type") == 0) {
			if (i_a) {
				Util_sscansdec(argv[++i], &CARTRIDGE_main.type);
				if (CARTRIDGE_main.type < 0 ||  CARTRIDGE_main.type >= CARTRIDGE_TYPE_COUNT)
					a_i = TRUE;
				else
					type_from_commandline = TRUE;
			}
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-cart2") == 0) {
			if (i_a) {
				Util_strlcpy(CARTRIDGE_piggyback.filename, argv[++i], sizeof(CARTRIDGE_piggyback.filename));
				if (!type2_from_commandline)
					CARTRIDGE_piggyback.type = CARTRIDGE_UNKNOWN;
			}
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-cart2-type") == 0) {
			if (i_a) {
				Util_sscansdec(argv[++i], &CARTRIDGE_piggyback.type);
				if (CARTRIDGE_piggyback.type < 0 ||  CARTRIDGE_piggyback.type >= CARTRIDGE_TYPE_COUNT)
					a_i = TRUE;
				else
					type2_from_commandline = TRUE;
			}
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-cart-autoreboot") == 0)
			CARTRIDGE_autoreboot = TRUE;
		else if (strcmp(argv[i], "-no-cart-autoreboot") == 0)
			CARTRIDGE_autoreboot = FALSE;
		else {
			if (strcmp(argv[i], "-help") == 0) {
				help_only = TRUE;
				Log_print("\t-cart <file>         Install cartridge (raw or CART format)");
				Log_print("\t-cart-type <num>     Set cartridge type (0..%i)", CARTRIDGE_TYPE_COUNT-1);
				Log_print("\t-cart2 <file>        Install piggyback cartridge");
				Log_print("\t-cart2-type <num>    Set piggyback cartridge type (0..%i)", CARTRIDGE_TYPE_COUNT-1);
				Log_print("\t-cart-autoreboot     Reboot when cartridge is inserted/removed");
				Log_print("\t-no-cart-autoreboot  Don't reboot after changing cartridge");
			}
			argv[j++] = argv[i];
		}

		if (a_m) {
			Log_print("Missing argument for '%s'", argv[i]);
			return FALSE;
		} else if (a_i) {
			Log_print("Invalid argument for '%s'", argv[--i]);
			return FALSE;
		}
	}
	*argc = j;

	if (help_only)
		return TRUE;

	/* If filename not given, we must reset the cartridge types. */
	if (CARTRIDGE_main.filename[0] == '\0')
		CARTRIDGE_main.type = CARTRIDGE_NONE;
	if (CARTRIDGE_piggyback.filename[0] == '\0')
		CARTRIDGE_piggyback.type = CARTRIDGE_NONE;

	InitInsert(&CARTRIDGE_main);
	if (CartIsPassthrough(CARTRIDGE_main.type))
		InitInsert(&CARTRIDGE_piggyback);

	return TRUE;
}

void CARTRIDGE_Exit(void)
{
	CARTRIDGE_Remove(); /* Removes both cartridges */
}

#ifndef BASIC

void CARTRIDGE_StateRead(UBYTE version)
{
	int saved_type = CARTRIDGE_NONE;
	char filename[FILENAME_MAX];

	/* Read the cart type from the file.  If there is no cart type, because we have
	   reached the end of the file, this will just default to CART_NONE */
	StateSav_ReadINT(&saved_type, 1);
	if (saved_type != CARTRIDGE_NONE) {
		StateSav_ReadFNAME(filename);
		if (filename[0]) {
			/* Insert the cartridge... */
			if (CARTRIDGE_Insert(filename) >= 0) {
				/* And set the type to the saved type, in case it was a raw cartridge image */
				CARTRIDGE_main.type = saved_type;
			}
		}
		if (version >= 7)
			/* Read the cartridge's state (current bank etc.). */
			StateSav_ReadINT(&CARTRIDGE_main.state, 1);
		if (version >= 8) {
			/* Read the cartridge's image type (raw, cart - for RAM carts updating on remove). */
			StateSav_ReadINT(&CARTRIDGE_main.raw, 1);
		}
	}
	else
		CARTRIDGE_main.type = saved_type;

	if (saved_type < 0) {
		/* Minus value indicates a piggyback cartridge present. */
		CARTRIDGE_main.type = -saved_type;
	
		StateSav_ReadINT(&saved_type, 1);
		StateSav_ReadFNAME(filename);
		if (filename[0]) {
			/* Insert the cartridge... */
			if (CARTRIDGE_Insert_Second(filename) >= 0) {
				/* And set the type to the saved type, in case it was a raw cartridge image */
				CARTRIDGE_piggyback.type = saved_type;
			}
		}
		if (version >= 7)
			/* Read the cartridge's state (current bank etc.). */
			StateSav_ReadINT(&CARTRIDGE_piggyback.state, 1);
		else {
			/* Savestate version 6 explicitely stored information about
			   the active cartridge. */
			int piggyback_active;
			StateSav_ReadINT(&piggyback_active, 1);
			if (piggyback_active)
				active_cart = &CARTRIDGE_piggyback;
			else
				active_cart = &CARTRIDGE_main;
			/* The "Determine active cartridge" code below makes no
			   sense when loading ver.6 savestates, because they
			   did not store the cartridge state. */
			return;
		}
		if (version >= 8) {
			/* Read the cartridge's image type (raw, cart - for RAM carts updating on remove). */
			StateSav_ReadINT(&CARTRIDGE_piggyback.raw, 1);
		}
	}

	/* Determine active cartridge (main or piggyback). */
	if (CartIsPassthrough(CARTRIDGE_main.type) && (CARTRIDGE_main.state & 0x0c) == 0x08)
		active_cart = &CARTRIDGE_piggyback;
	else
		active_cart = &CARTRIDGE_main;

	MapActiveCart();
}

void CARTRIDGE_StateSave(void)
{
	int cart_save = CARTRIDGE_main.type;
	
	if (CARTRIDGE_piggyback.type != CARTRIDGE_NONE)
		/* Save the cart type as negative, to indicate to CARTStateRead that there is a 
		   second cartridge */
		cart_save = -cart_save;
	
	/* Save the cartridge type, or CARTRIDGE_NONE if there isn't one...*/
	StateSav_SaveINT(&cart_save, 1);
	if (CARTRIDGE_main.type != CARTRIDGE_NONE) {
		StateSav_SaveFNAME(CARTRIDGE_main.filename);
		StateSav_SaveINT(&CARTRIDGE_main.state, 1);
		StateSav_SaveINT(&CARTRIDGE_main.raw, 1);
	}

	if (CARTRIDGE_piggyback.type != CARTRIDGE_NONE) {
		/* Save the second cartridge type and name*/
		StateSav_SaveINT(&CARTRIDGE_piggyback.type, 1);
		StateSav_SaveFNAME(CARTRIDGE_piggyback.filename);
		StateSav_SaveINT(&CARTRIDGE_piggyback.state, 1);
		StateSav_SaveINT(&CARTRIDGE_piggyback.raw, 1);
	}
}

#endif

/*
vim:ts=4:sw=4:
*/

