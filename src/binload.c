/*
 * binload.c - load a binary executable file
 *
 * Copyright (C) 1995-1998 David Firth
 * Copyright (C) 1998-2005 Atari800 development team (see DOC/CREDITS)
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
#include "atari.h"
#include "binload.h"
#include "cpu.h"
#include "devices.h"
#include "esc.h"
#include "log.h"
#include "memory.h"
#include "sio.h"

int BINLOAD_start_binloading = FALSE;
int BINLOAD_loading_basic = 0;
int BINLOAD_slow_xex_loading = FALSE;
FIL BINLOAD_bin_file;
int BINLOAD_bin_file_open = FALSE;

/* These variables are for slow XEX loading only. */

/* Number of CPU instructions elapsed since last loaded byte. */
static unsigned int instr_elapsed = 0;
int BINLOAD_wait_active=FALSE;
/* Start and end address of the currently loaded segment. */
static UWORD from = 0;
static UWORD to = 0;
/* Inticates that the next call to loader_cont will overwrite INITAD. */
static int init2e3 = FALSE;
/* Indicates that we are currently not during loading of a segment. */
static int segfinished = TRUE;
int BINLOAD_pause_loading;

/* Read a word from file */
static int read_word(void) {
	if (!BINLOAD_bin_file_open) {
		Log_print("binload: not open BIN file");
		return -1;
	}
	UBYTE buf[2];
	UINT br;
	if (f_read(&BINLOAD_bin_file, buf, 2, &br) != FR_OK) {
		f_close(&BINLOAD_bin_file);
		BINLOAD_bin_file_open = FALSE;
		if (BINLOAD_start_binloading) {
			BINLOAD_start_binloading = FALSE;
			Log_print("binload: not valid BIN file");
			return -1;
		}
		CPU_regPC = MEMORY_dGetWordAligned(0x2e0);
		return -1;
	}
	return buf[0] + (buf[1] << 8);
}

/* Start or continue loading */
static void loader_cont(void)
{
	if (BINLOAD_start_binloading) {
		MEMORY_dPutByte(0x244, 0);
		MEMORY_dPutByte(0x09, 1);
	}
	else
		CPU_regS += 2;	/* pop ESC code */

	if (init2e3)
		MEMORY_dPutByte(0x2e3, 0xd7);
	init2e3=FALSE;
	do {
		if((!BINLOAD_wait_active || !BINLOAD_slow_xex_loading) && segfinished){
			int temp;
			do
				temp = read_word();
			while (temp == 0xffff);
			if (temp < 0)
				return;
			from = (UWORD) temp;

			temp = read_word();
			if (temp < 0)
				return;
			to = (UWORD) temp;

			if (BINLOAD_start_binloading) {
				MEMORY_dPutWordAligned(0x2e0, from);
				BINLOAD_start_binloading = FALSE;
			}
			to++;
			segfinished = FALSE;
		}
		do {
			int byte;
			if (BINLOAD_slow_xex_loading) {
				instr_elapsed++;
				if ((instr_elapsed < 300) || BINLOAD_pause_loading) {
					CPU_regS--;
					ESC_Add((UWORD) (0x100 + CPU_regS), ESC_BINLOADER_CONT, loader_cont);
					CPU_regS--;
					CPU_regPC = CPU_regS + 1 + 0x100;
					BINLOAD_wait_active = TRUE;
					return;
				}
				instr_elapsed = 0;
				BINLOAD_wait_active = FALSE;
			}
			byte = _fgetc(&BINLOAD_bin_file);
			if (byte == EOF) {
				f_close(&BINLOAD_bin_file);
				BINLOAD_bin_file_open = FALSE;
				CPU_regPC = MEMORY_dGetWordAligned(0x2e0);
				if (MEMORY_dGetByte(0x2e3) != 0xd7) {
					/* run INIT routine which RTSes directly to RUN routine */
					CPU_regPC--;
					MEMORY_dPutByte(0x0100 + CPU_regS--, CPU_regPC >> 8);		/* high */
					MEMORY_dPutByte(0x0100 + CPU_regS--, CPU_regPC & 0xff);	/* low */
					CPU_regPC = MEMORY_dGetWordAligned(0x2e2);
				}
				return;
			}
			MEMORY_PutByte(from, (UBYTE) byte);
			from++;
		} while (from != to);
		segfinished = TRUE;
	} while (MEMORY_dGetByte(0x2e3) == 0xd7);

	CPU_regS--;
	ESC_Add((UWORD) (0x100 + CPU_regS), ESC_BINLOADER_CONT, loader_cont);
	CPU_regS--;
	MEMORY_dPutByte(0x0100 + CPU_regS--, 0x01);	/* high */
	MEMORY_dPutByte(0x0100 + CPU_regS, CPU_regS + 1);	/* low */
	CPU_regS--;
	CPU_regPC = MEMORY_dGetWordAligned(0x2e2);
	CPU_SetC;

	MEMORY_dPutByte(0x0300, 0x31);	/* for "Studio Dream" */
	init2e3 = TRUE;
}

/* Fake boot sector to call loader_cont at boot time */
int BINLOAD_LoaderStart(UBYTE *buffer)
{
	buffer[0] = 0x00;	/* ignored */
	buffer[1] = 0x01;	/* one boot sector */
	buffer[2] = 0x00;	/* start at memory location 0x0700 */
	buffer[3] = 0x07;
	buffer[4] = 0x77;	/* reset reboots (0xe477 = Atari OS Coldstart) */
	buffer[5] = 0xe4;
	buffer[6] = 0xf2;	/* ESC */
	buffer[7] = ESC_BINLOADER_CONT;
	ESC_Add(0x706, ESC_BINLOADER_CONT, loader_cont);
	BINLOAD_wait_active = FALSE;
	init2e3 = TRUE;
	segfinished = TRUE;
	return 'C';
}

/* Load BIN file, returns TRUE if ok */
int BINLOAD_Loader(const char *filename)
{
	UBYTE buf[2];
	if (BINLOAD_bin_file_open) {		/* close previously open file */
		f_close(&BINLOAD_bin_file);
		BINLOAD_bin_file_open = FALSE;
		BINLOAD_loading_basic = 0;
	}
	if (Atari800_machine_type == Atari800_MACHINE_5200) {
		Log_print("binload: can't run Atari programs directly on the 5200");
#ifdef LIBATARI800
		CPU_cim_encountered = 1;
#endif
		return FALSE;
	}
	if (f_open(&BINLOAD_bin_file, filename, FA_READ) != FR_OK) {	/* open */
		Log_print("binload: can't open \"%s\"", filename);
		return FALSE;
	}
	BINLOAD_bin_file_open = TRUE;
	/* Avoid "BOOT ERROR" when loading a BASIC program */
	if (SIO_drive_status[0] == SIO_NO_DISK)
		SIO_DisableDrive(1);
	UINT rb;
	if (fread(&BINLOAD_bin_file, buf, 2, &rb) == FR_OK) {
		if (buf[0] == 0xff && buf[1] == 0xff) {
			BINLOAD_start_binloading = TRUE; /* force SIO to call BINLOAD_LoaderStart at boot */
			Atari800_Coldstart();             /* reboot */
			return TRUE;
		}
		else if (buf[0] == 0 && buf[1] == 0) {
			BINLOAD_loading_basic = BINLOAD_LOADING_BASIC_SAVED;
			ESC_UpdatePatches();
			Atari800_Coldstart();
			return TRUE;
		}
		else if (buf[0] >= '0' && buf[0] <= '9') {
			BINLOAD_loading_basic = BINLOAD_LOADING_BASIC_LISTED;
			ESC_UpdatePatches();
			Atari800_Coldstart();
			return TRUE;
		}
	}
	f_close(&BINLOAD_bin_file);
	BINLOAD_bin_file_open = FALSE;
	Log_print("binload: \"%s\" not recognized as a DOS or BASIC program", filename);
	return FALSE;
}
