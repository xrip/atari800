/*
 * monitor.c - Implements a builtin system monitor for debugging
 *
 * Copyright (C) 1995-1998 David Firth
 * Copyright (C) 1998-2010 Atari800 development team (see DOC/CREDITS)
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

#define _POSIX_C_SOURCE 200112L /* for snprintf */

#include "config.h"
#include "ff.h"

#include <string.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <math.h>
#ifdef HAVE_TERMIOS_H
# include <termios.h>
#endif
#ifdef GWINSZ_IN_SYS_IOCTL
# include <sys/ioctl.h>
#endif
#include "binload.h"
#include "antic.h"
#include "atari.h"
#include "cpu.h"
#include "gtia.h"
#include "memory.h"
#include "cartridge.h"
#include "monitor.h"
#include "pia.h"
#include "pokey.h"
#include "util.h"
#ifdef STEREO_SOUND
#include "pokeysnd.h"
#endif
#include "platform.h"
#include "statesav.h"

#ifdef MONITOR_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#ifdef __PLUS

#include <stdarg.h>
#include "misc_win.h"

FILE *mon_output, *mon_input;

void monitor_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(mon_output, format, args);
	va_end(args);
}

#define printf            monitor_printf
#define puts(s)           fputs(s, mon_output)
#define putchar(c)        fputc(c, mon_output)
#define perror(filename)  printf("%s: %s\n", filename, strerror(errno))

#undef stdout
#define stdout mon_output

#undef stdin
#define stdin mon_input

#define PLUS_EXIT_MONITOR Misc_FreeMonitorConsole(mon_output, mon_input)

#else /* __PLUS */

#define PLUS_EXIT_MONITOR

#endif /* __PLUS */

UBYTE *trainer_memory = NULL;
UBYTE *trainer_flags = NULL;

#ifdef MONITOR_TRACE
FILE *MONITOR_trace_file = NULL;
#endif

#ifdef MONITOR_HINTS

typedef struct {
	char *name;
	UWORD addr;
} symtable_rec;

 /* Symbol names taken from:
    - XL OS sources, Atari Home Computer System - Operating System Manual - XL Addendum
    - BASIC sources, The Atari BASIC Source Book
    Symbols must be sorted by address. If the address has different names
    when reading/writing to it, put the read name first. */

static const symtable_rec symtable_builtin[] = {
	{"LNFLG",   0x0000}, {"NGFLAG",  0x0001}, {"CASINI",  0x0002}, {"CASINI+1",0x0003},
	{"RAMLO",   0x0004}, {"RAMLO+1", 0x0005}, {"TRAMSZ",  0x0006}, {"CMCMD",   0x0007},
	{"WARMST",  0x0008}, {"BOOT?",   0x0009}, {"DOSVEC",  0x000a}, {"DOSVEC+1",0x000b},
	{"DOSINI",  0x000c}, {"DOSINI+1",0x000d}, {"APPMHI",  0x000e}, {"APPMHI+1",0x000f},
	{"POKMSK",  0x0010}, {"BRKKEY",  0x0011}, {"RTCLOK",  0x0012}, {"RTCLOK+1",0x0013},
	{"RTCLOK+2",0x0014}, {"BUFADR",  0x0015}, {"BUFADR+1",0x0016}, {"ICCOMT",  0x0017},
	{"DSKFMS",  0x0018}, {"DSKFMS+1",0x0019}, {"DSKUTL",  0x001a}, {"DSKUTL+1",0x001b},
	{"ABUFPT",  0x001c}, {"ABUFPT+1",0x001d}, {"ABUFPT+2",0x001e}, {"ABUFPT+3",0x001f},
	{"ICHIDZ",  0x0020}, {"ICDNOZ",  0x0021}, {"ICCOMZ",  0x0022}, {"ICSTAZ",  0x0023},
	{"ICBALZ",  0x0024}, {"ICBAHZ",  0x0025}, {"ICPTLZ",  0x0026}, {"ICPTHZ",  0x0027},
	{"ICBLLZ",  0x0028}, {"ICBLHZ",  0x0029}, {"ICAX1Z",  0x002a}, {"ICAX2Z",  0x002b},
	{"ICSPRZ",  0x002c}, {"ICSPRZ+1",0x002d}, {"ICIDNO",  0x002e}, {"CIOCHR",  0x002f},
	{"STATUS",  0x0030}, {"CHKSUM",  0x0031}, {"BUFRLO",  0x0032}, {"BUFRHI",  0x0033},
	{"BFENLO",  0x0034}, {"BFENHI",  0x0035}, {"LTEMP",   0x0036}, {"LTEMP+1", 0x0037},
	{"BUFRFL",  0x0038}, {"RECVDN",  0x0039}, {"XMTDON",  0x003a}, {"CHKSNT",  0x003b},
	{"NOCKSM",  0x003c}, {"BPTR",    0x003d}, {"FTYPE",   0x003e}, {"FEOF",    0x003f},
	{"FREQ",    0x0040}, {"SOUNDR",  0x0041}, {"CRITIC",  0x0042}, {"FMSZPG",  0x0043},
	{"FMSZPG+1",0x0044}, {"FMSZPG+2",0x0045}, {"FMSZPG+3",0x0046}, {"FMSZPG+4",0x0047},
	{"FMSZPG+5",0x0048}, {"FMSZPG+6",0x0049}, {"ZCHAIN",  0x004a}, {"ZCHAIN+1",0x004b},
	{"DSTAT",   0x004c}, {"ATRACT",  0x004d}, {"DRKMSK",  0x004e}, {"COLRSH",  0x004f},
	{"TMPCHR",  0x0050}, {"HOLD1",   0x0051}, {"LMARGN",  0x0052}, {"RMARGN",  0x0053},
	{"ROWCRS",  0x0054}, {"COLCRS",  0x0055}, {"COLCRS+1",0x0056}, {"DINDEX",  0x0057},
	{"SAVMSC",  0x0058}, {"SAVMSC+1",0x0059}, {"OLDROW",  0x005a}, {"OLDCOL",  0x005b},
	{"OLDCOL+1",0x005c}, {"OLDCHR",  0x005d}, {"OLDADR",  0x005e}, {"OLDADR+1",0x005f},
	{"FKDEF",   0x0060}, {"FKDEF+1", 0x0061}, {"PALNTS",  0x0062}, {"LOGCOL",  0x0063},
	{"ADRESS",  0x0064}, {"ADRESS+1",0x0065}, {"TOADR",   0x0066}, {"TOADR+1", 0x0067},
	{"SAVADR",  0x0068}, {"SAVADR+1",0x0069}, {"RAMTOP",  0x006a}, {"BUFCNT",  0x006b},
	{"BUFSTR",  0x006c}, {"BUFSTR+1",0x006d}, {"BITMSK",  0x006e}, {"SHFAMT",  0x006f},
	{"ROWAC",   0x0070}, {"ROWAC+1", 0x0071}, {"COLAC",   0x0072}, {"COLAC+1", 0x0073},
	{"ENDPT",   0x0074}, {"ENDPT+1", 0x0075}, {"DELTAR",  0x0076}, {"DELTAC",  0x0077},
	{"DELTAC+1",0x0078}, {"KEYDEF",  0x0079}, {"KEYDEF+1",0x007a}, {"SWPFLG",  0x007b},
	{"HOLDCH",  0x007c}, {"INSDAT",  0x007d}, {"COUNTR",  0x007e}, {"COUNTR+1",0x007f},

	{"LOMEM",   0x0080}, {"LOMEM+1", 0x0081}, {"VNTP",    0x0082}, {"VNTP+1",  0x0083},
	{"VNTD",    0x0084}, {"VNTD+1",  0x0085}, {"VVTP",    0x0086}, {"VVTP+1",  0x0087},
	{"STMTAB",  0x0088}, {"STMTAB+1",0x0089}, {"STMCUR",  0x008a}, {"STMCUR+1",0x008b},
	{"STARP",   0x008c}, {"STARP+1", 0x008d}, {"RUNSTK",  0x008e}, {"RUNSTK+1",0x008f},
	{"MEMTOP",  0x0090}, {"MEMTOP+1",0x0091}, {"MEOLFLG", 0x0092},
	{"COX",     0x0094}, {"POKADR",  0x0095}, {"POKADR+1",0x0096}, {"SVESA",   0x0097},
	{"SVESA+1", 0x0098}, {"MVFA",    0x0099}, {"MVFA+1",  0x009a}, {"MVTA",    0x009b},
	{"MVTA+1",  0x009c}, {"CPC",     0x009d}, {"CPC+1",   0x009e}, {"LLNGTH",  0x009f},
	{"TSLNUM",  0x00a0}, {"TSLNUM+1",0x00a1}, {"MVLNG",   0x00a2}, {"MVLNG+1", 0x00a3},
	{"ECSIZE",  0x00a4}, {"ECSIZE+1",0x00a5}, {"DIRFLG",  0x00a6}, {"STMLBD",  0x00a7},
	{"STINDEX", 0x00a8}, {"OPSTKX",  0x00a9}, {"ARSTKX",  0x00aa}, {"EXSVOP",  0x00ab},
	{"EXSVPR",  0x00ac}, {"LELNUM",  0x00ad}, {"LELNUM+1",0x00ae}, {"STENUM",  0x00af},
	{"COMCNT",  0x00b0}, {"ADFLAG",  0x00b1}, {"SVDISP",  0x00b2}, {"ONLOOP",  0x00b3},
	{"ENTDTD",  0x00b4}, {"LISTDTD", 0x00b5}, {"DATAD",   0x00b6}, {"DATALN",  0x00b7},
	{"DATALN+1",0x00b8}, {"ERRNUM",  0x00b9}, {"STOPLN",  0x00ba}, {"STOPLN+1",0x00bb},
	{"TRAPLN",  0x00bc}, {"TRAPLN+1",0x00bd}, {"SAVCUR",  0x00be}, {"SAVCUR+1",0x00bf},
	{"IOCMD",   0x00c0}, {"IODVC",   0x00c1}, {"PROMPT",  0x00c2}, {"ERRSAV",  0x00c3},
	{"TEMPA",   0x00c4}, {"TEMPA+1", 0x00c5}, {"ZTEMP2",  0x00c6}, {"ZTEMP2+1",0x00c7},
	{"COLOR",   0x00c8}, {"PTABW",   0x00c9}, {"LOADFLG", 0x00ca},
	{"VTYPE",   0x00d2}, {"VNUM",    0x00d3},
	{"FR0",     0x00d4}, {"FR0+1",   0x00d5}, {"FR0+2",   0x00d6}, {"FR0+3",   0x00d7},
	{"FR0+4",   0x00d8}, {"FR0+5",   0x00d9}, {"FRE",     0x00da}, {"FRE+1",   0x00db},
	{"FRE+2",   0x00dc}, {"FRE+3",   0x00dd}, {"FRE+4",   0x00de}, {"FRE+5",   0x00df},
	{"FR1",     0x00e0}, {"FR1+1",   0x00e1}, {"FR1+2",   0x00e2}, {"FR1+3",   0x00e3},
	{"FR1+4",   0x00e4}, {"FR1+5",   0x00e5}, {"FR2",     0x00e6}, {"FR2+1",   0x00e7},
	{"FR2+2",   0x00e8}, {"FR2+3",   0x00e9}, {"FR2+4",   0x00ea}, {"FR2+5",   0x00eb},
	{"FRX",     0x00ec}, {"EEXP",    0x00ed}, {"NSIGN",   0x00ee}, {"ESIGN",   0x00ef},
	{"FCHRFLG", 0x00f0}, {"DIGRT",   0x00f1}, {"CIX",     0x00f2}, {"INBUFF",  0x00f3},
	{"INBUFF+1",0x00f4}, {"ZTEMP1",  0x00f5}, {"ZTEMP1+1",0x00f6}, {"ZTEMP4",  0x00f7},
	{"ZTEMP4+1",0x00f8}, {"ZTEMP3",  0x00f9}, {"ZTEMP3+1",0x00fa}, {"RADFLG",  0x00fb},
	{"FLPTR",   0x00fc}, {"FLPTR+1", 0x00fd}, {"FPTR2",   0x00fe}, {"FPTR2+1", 0x00ff},

	{"VDSLST",  0x0200}, {"VDSLST+1",0x0201}, {"VPRCED",  0x0202}, {"VPRCED+1",0x0203},
	{"VINTER",  0x0204}, {"VINTER+1",0x0205}, {"VBREAK",  0x0206}, {"VBREAK+1",0x0207},
	{"VKEYBD",  0x0208}, {"VKEYBD+1",0x0209}, {"VSERIN",  0x020a}, {"VSERIN+1",0x020b},
	{"VSEROR",  0x020c}, {"VSEROR+1",0x020d}, {"VSEROC",  0x020e}, {"VSEROC+1",0x020f},
	{"VTIMR1",  0x0210}, {"VTIMR1+1",0x0211}, {"VTIMR2",  0x0212}, {"VTIMR2+1",0x0213},
	{"VTIMR4",  0x0214}, {"VTIMR4+1",0x0215}, {"VIMIRQ",  0x0216}, {"VIMIRQ+1",0x0217},
	{"CDTMV1",  0x0218}, {"CDTMV1+1",0x0219}, {"CDTMV2",  0x021a}, {"CDTMV2+1",0x021b},
	{"CDTMV3",  0x021c}, {"CDTMV3+1",0x021d}, {"CDTMV4",  0x021e}, {"CDTMV4+1",0x021f},
	{"CDTMV5",  0x0220}, {"CDTMV5+1",0x0221}, {"VVBLKI",  0x0222}, {"VVBLKI+1",0x0223},
	{"VVBLKD",  0x0224}, {"VVBLKD+1",0x0225}, {"CDTMA1",  0x0226}, {"CDTMA1+1",0x0227},
	{"CDTMA2",  0x0228}, {"CDTMA2+1",0x0229}, {"CDTMF3",  0x022a}, {"SRTIMR",  0x022b},
	{"CDTMF4",  0x022c}, {"INTEMP",  0x022d}, {"CDTMF5",  0x022e}, {"SDMCTL",  0x022f},
	{"SDLSTL",  0x0230}, {"SDLSTH",  0x0231}, {"SSKCTL",  0x0232}, {"LCOUNT",  0x0233},
	{"LPENH",   0x0234}, {"LPENV",   0x0235}, {"BRKKY",   0x0236}, {"BRKKY+1", 0x0237},
	{"VPIRQ",   0x0238}, {"VPIRQ+1", 0x0239}, {"CDEVIC",  0x023a}, {"CCOMND",  0x023b},
	{"CAUX1",   0x023c}, {"CAUX2",   0x023d}, {"TEMP",    0x023e}, {"ERRFLG",  0x023f},
	{"DFLAGS",  0x0240}, {"DBSECT",  0x0241}, {"BOOTAD",  0x0242}, {"BOOTAD+1",0x0243},
	{"COLDST",  0x0244}, {"RECLEN",  0x0245}, {"DSKTIM",  0x0246}, {"PDVMSK",  0x0247},
	{"SHPDVS",  0x0248}, {"PDIMSK",  0x0249}, {"RELADR",  0x024a}, {"RELADR+1",0x024b},
	{"PPTMPA",  0x024c}, {"PPTMPX",  0x024d}, {"CHSALT",  0x026b},
	{"VSFLAG",  0x026c}, {"KEYDIS",  0x026d}, {"FINE",    0x026e}, {"GPRIOR",  0x026f},
	{"PADDL0",  0x0270}, {"PADDL1",  0x0271}, {"PADDL2",  0x0272}, {"PADDL3",  0x0273},
	{"PADDL4",  0x0274}, {"PADDL5",  0x0275}, {"PADDL6",  0x0276}, {"PADDL7",  0x0277},
	{"STICK0",  0x0278}, {"STICK1",  0x0279}, {"STICK2",  0x027a}, {"STICK3",  0x027b},
	{"PTRIG0",  0x027c}, {"PTRIG1",  0x027d}, {"PTRIG2",  0x027e}, {"PTRIG3",  0x027f},
	{"PTRIG4",  0x0280}, {"PTRIG5",  0x0281}, {"PTRIG6",  0x0282}, {"PTRIG7",  0x0283},
	{"STRIG0",  0x0284}, {"STRIG1",  0x0285}, {"STRIG2",  0x0286}, {"STRIG3",  0x0287},
	{"HIBYTE",  0x0288}, {"WMODE",   0x0289}, {"BLIM",    0x028a}, {"IMASK",   0x028b},
	{"JVECK",   0x028c}, {"JVECK+1", 0x028d}, {"NEWADR",  0x028e}, {"NEWADR+1",0x028f},
	{"TXTROW",  0x0290}, {"TXTCOL",  0x0291}, {"TXTCOL+1",0x0292}, {"TINDEX",  0x0293},
	{"TXTMSC",  0x0294}, {"TXTMSC+1",0x0295}, {"TXTOLD",  0x0296}, {"TXTOLD+1",0x0297},
	{"TXTOLD+2",0x0298}, {"TXTOLD+3",0x0299}, {"TXTOLD+4",0x029a}, {"TXTOLD+5",0x029b},
	{"CRETRY",  0x029c}, {"HOLD3",   0x029d}, {"SUBTMP",  0x029e}, {"HOLD2",   0x029f},
	{"DMASK",   0x02a0}, {"TMPLBT",  0x02a1}, {"ESCFLG",  0x02a2}, {"TABMAP",  0x02a3},
	{"TABMAP+1",0x02a4}, {"TABMAP+2",0x02a5}, {"TABMAP+3",0x02a6}, {"TABMAP+4",0x02a7},
	{"TABMAP+5",0x02a8}, {"TABMAP+6",0x02a9}, {"TABMAP+7",0x02aa}, {"TABMAP+8",0x02ab},
	{"TABMAP+9",0x02ac}, {"TABMAP+A",0x02ad}, {"TABMAP+B",0x02ae}, {"TABMAP+C",0x02af},
	{"TABMAP+D",0x02b0}, {"TABMAP+E",0x02b1}, {"LOGMAP",  0x02b2}, {"LOGMAP+1",0x02b3},
	{"LOGMAP+2",0x02b4}, {"LOGMAP+3",0x02b5}, {"INVFLG",  0x02b6}, {"FILFLG",  0x02b7},
	{"TMPROW",  0x02b8}, {"TMPCOL",  0x02b9}, {"TMPCOL+1",0x02ba}, {"SCRFLG",  0x02bb},
	{"HOLD4",   0x02bc}, {"DRETRY",  0x02bd}, {"SHFLOK",  0x02be}, {"BOTSCR",  0x02bf},
	{"PCOLR0",  0x02c0}, {"PCOLR1",  0x02c1}, {"PCOLR2",  0x02c2}, {"PCOLR3",  0x02c3},
	{"COLOR0",  0x02c4}, {"COLOR1",  0x02c5}, {"COLOR2",  0x02c6}, {"COLOR3",  0x02c7},
	{"COLOR4",  0x02c8}, {"RUNADR",  0x02c9}, {"RUNADR+1",0x02ca}, {"HIUSED",  0x02cb},
	{"HIUSED+1",0x02cc}, {"ZHIUSE",  0x02cd}, {"ZHIUSE+1",0x02ce}, {"GBYTEA",  0x02cf},
	{"GBYTEA+1",0x02d0}, {"LOADAD",  0x02d1}, {"LOADAD+1",0x02d2}, {"ZLOADA",  0x02d3},
	{"ZLOADA+1",0x02d4}, {"DSCTLN",  0x02d5}, {"DSCTLN+1",0x02d6}, {"ACMISR",  0x02d7},
	{"ACMISR+1",0x02d8}, {"KRPDEL",  0x02d9}, {"KEYREP",  0x02da}, {"NOCLIK",  0x02db},
	{"HELPFG",  0x02dc}, {"DMASAV",  0x02dd}, {"PBPNT",   0x02de}, {"PBUFSZ",  0x02df},
	{"RUNAD",   0x02e0}, {"RUNAD+1", 0x02e1}, {"INITAD",  0x02e2}, {"INITAD+1",0x02e3},
	{"RAMSIZ",  0x02e4}, {"MEMTOP",  0x02e5}, {"MEMTOP+1",0x02e6}, {"MEMLO",   0x02e7},
	{"MEMLO+1", 0x02e8}, {"HNDLOD",  0x02e9}, {"DVSTAT",  0x02ea}, {"DVSTAT+1",0x02eb},
	{"DVSTAT+2",0x02ec}, {"DVSTAT+3",0x02ed}, {"CBAUDL",  0x02ee}, {"CBAUDH",  0x02ef},
	{"CRSINH",  0x02f0}, {"KEYDEL",  0x02f1}, {"CH1",     0x02f2}, {"CHACT",   0x02f3},
	{"CHBAS",   0x02f4}, {"NEWROW",  0x02f5}, {"NEWCOL",  0x02f6}, {"NEWCOL+1",0x02f7},
	{"ROWINC",  0x02f8}, {"COLINC",  0x02f9}, {"CHAR",    0x02fa}, {"ATACHR",  0x02fb},
	{"CH",      0x02fc}, {"FILDAT",  0x02fd}, {"DSPFLG",  0x02fe}, {"SSFLAG",  0x02ff},

	{"DDEVIC",  0x0300}, {"DUNIT",   0x0301}, {"DCOMND",  0x0302}, {"DSTATS",  0x0303},
	{"DBUFLO",  0x0304}, {"DBUFHI",  0x0305}, {"DTIMLO",  0x0306}, {"DUNUSE",  0x0307},
	{"DBYTLO",  0x0308}, {"DBYTHI",  0x0309}, {"DAUX1",   0x030a}, {"DAUX2",   0x030b},
	{"TIMER1",  0x030c}, {"TIMER1+1",0x030d}, {"ADDCOR",  0x030e}, {"CASFLG",  0x030f},
	{"TIMER2",  0x0310}, {"TIMER2+1",0x0311}, {"TEMP1",   0x0312}, {"TEMP2",   0x0313},
	{"PTIMOT",  0x0314}, {"TEMP3",   0x0315}, {"SAVIO",   0x0316}, {"TIMFLG",  0x0317},
	{"STACKP",  0x0318}, {"TSTAT",   0x0319}, {"HATABS",  0x031a}, /* HATABS 1-34 */
	{"PUPBT1",  0x033d}, {"PUPBT2",  0x033e}, {"PUPBT3",  0x033f},
	{"B0-ICHID",0x0340}, {"B0-ICDNO",0x0341}, {"B0-ICCOM",0x0342}, {"B0-ICSTA",0x0343},
	{"B0-ICBAL",0x0344}, {"B0-ICBAH",0x0345}, {"B0-ICPTL",0x0346}, {"B0-ICPTH",0x0347},
	{"B0-ICBLL",0x0348}, {"B0-ICBLH",0x0349}, {"B0-ICAX1",0x034a}, {"B0-ICAX2",0x034b},
	{"B0-ICAX3",0x034c}, {"B0-ICAX4",0x034d}, {"B0-ICAX5",0x034e}, {"B0-ICAX6",0x034f},
	{"B1-ICHID",0x0350}, {"B1-ICDNO",0x0351}, {"B1-ICCOM",0x0352}, {"B1-ICSTA",0x0353},
	{"B1-ICBAL",0x0354}, {"B1-ICBAH",0x0355}, {"B1-ICPTL",0x0356}, {"B1-ICPTH",0x0357},
	{"B1-ICBLL",0x0358}, {"B1-ICBLH",0x0359}, {"B1-ICAX1",0x035a}, {"B1-ICAX2",0x035b},
	{"B1-ICAX3",0x035c}, {"B1-ICAX4",0x035d}, {"B1-ICAX5",0x035e}, {"B1-ICAX6",0x035f},
	{"B2-ICHID",0x0360}, {"B2-ICDNO",0x0361}, {"B2-ICCOM",0x0362}, {"B2-ICSTA",0x0363},
	{"B2-ICBAL",0x0364}, {"B2-ICBAH",0x0365}, {"B2-ICPTL",0x0366}, {"B2-ICPTH",0x0367},
	{"B2-ICBLL",0x0368}, {"B2-ICBLH",0x0369}, {"B2-ICAX1",0x036a}, {"B2-ICAX2",0x036b},
	{"B2-ICAX3",0x036c}, {"B2-ICAX4",0x036d}, {"B2-ICAX5",0x036e}, {"B2-ICAX6",0x036f},
	{"B3-ICHID",0x0370}, {"B3-ICDNO",0x0371}, {"B3-ICCOM",0x0372}, {"B3-ICSTA",0x0373},
	{"B3-ICBAL",0x0374}, {"B3-ICBAH",0x0375}, {"B3-ICPTL",0x0376}, {"B3-ICPTH",0x0377},
	{"B3-ICBLL",0x0378}, {"B3-ICBLH",0x0379}, {"B3-ICAX1",0x037a}, {"B3-ICAX2",0x037b},
	{"B3-ICAX3",0x037c}, {"B3-ICAX4",0x037d}, {"B3-ICAX5",0x037e}, {"B3-ICAX6",0x037f},
	{"B4-ICHID",0x0380}, {"B4-ICDNO",0x0381}, {"B4-ICCOM",0x0382}, {"B4-ICSTA",0x0383},
	{"B4-ICBAL",0x0384}, {"B4-ICBAH",0x0385}, {"B4-ICPTL",0x0386}, {"B4-ICPTH",0x0387},
	{"B4-ICBLL",0x0388}, {"B4-ICBLH",0x0389}, {"B4-ICAX1",0x038a}, {"B4-ICAX2",0x038b},
	{"B4-ICAX3",0x038c}, {"B4-ICAX4",0x038d}, {"B4-ICAX5",0x038e}, {"B4-ICAX6",0x038f},
	{"B5-ICHID",0x0390}, {"B5-ICDNO",0x0391}, {"B5-ICCOM",0x0392}, {"B5-ICSTA",0x0393},
	{"B5-ICBAL",0x0394}, {"B5-ICBAH",0x0395}, {"B5-ICPTL",0x0396}, {"B5-ICPTH",0x0397},
	{"B5-ICBLL",0x0398}, {"B5-ICBLH",0x0399}, {"B5-ICAX1",0x039a}, {"B5-ICAX2",0x039b},
	{"B5-ICAX3",0x039c}, {"B5-ICAX4",0x039d}, {"B5-ICAX5",0x039e}, {"B5-ICAX6",0x039f},
	{"B6-ICHID",0x03a0}, {"B6-ICDNO",0x03a1}, {"B6-ICCOM",0x03a2}, {"B6-ICSTA",0x03a3},
	{"B6-ICBAL",0x03a4}, {"B6-ICBAH",0x03a5}, {"B6-ICPTL",0x03a6}, {"B6-ICPTH",0x03a7},
	{"B6-ICBLL",0x03a8}, {"B6-ICBLH",0x03a9}, {"B6-ICAX1",0x03aa}, {"B6-ICAX2",0x03ab},
	{"B6-ICAX3",0x03ac}, {"B6-ICAX4",0x03ad}, {"B6-ICAX5",0x03ae}, {"B6-ICAX6",0x03af},
	{"B7-ICHID",0x03b0}, {"B7-ICDNO",0x03b1}, {"B7-ICCOM",0x03b2}, {"B7-ICSTA",0x03b3},
	{"B7-ICBAL",0x03b4}, {"B7-ICBAH",0x03b5}, {"B7-ICPTL",0x03b6}, {"B7-ICPTH",0x03b7},
	{"B7-ICBLL",0x03b8}, {"B7-ICBLH",0x03b9}, {"B7-ICAX1",0x03ba}, {"B7-ICAX2",0x03bb},
	{"B7-ICAX3",0x03bc}, {"B7-ICAX4",0x03bd}, {"B7-ICAX5",0x03be}, {"B7-ICAX6",0x03bf},
	{"PRNBUF",  0x03c0}, /* PRNBUF 1-39 */
	{"SUPERF",  0x03e8}, {"CKEY",    0x03e9}, {"CASSBT",  0x03ea}, {"CARTCK",  0x03eb},
	{"DERRF",   0x03ec}, {"ACMVAR",  0x03ed}, /* ACMVAR 1-10 */
	{"BASICF",  0x03f8}, {"MINTLK",  0x03f9}, {"GINTLK",  0x03fa}, {"CHLINK",  0x03fb},
	{"CHLINK+1",0x03fc}, {"CASBUF",  0x03fd},

	{"R-CARTCS",0x9ffa}, {"R-CARTCS+1",0x9ffb},
	{"R-CART",  0x9ffc}, {"R-CARTFG",  0x9ffd}, {"R-CARTAD",0x9ffe}, {"R-CARTAD+1",0x9fff},

	{"CARTCS",  0xbffa}, {"CARTCS+1",0xbffb},
	{"CART",    0xbffc}, {"CARTFG",  0xbffd}, {"CARTAD",  0xbffe}, {"CARTAD+1",0xbfff},

	{"M0PF",  0xd000}, {"HPOSP0",0xd000}, {"M1PF",  0xd001}, {"HPOSP1",0xd001},
	{"M2PF",  0xd002}, {"HPOSP2",0xd002}, {"M3PF",  0xd003}, {"HPOSP3",0xd003},
	{"P0PF",  0xd004}, {"HPOSM0",0xd004}, {"P1PF",  0xd005}, {"HPOSM1",0xd005},
	{"P2PF",  0xd006}, {"HPOSM2",0xd006}, {"P3PF",  0xd007}, {"HPOSM3",0xd007},
	{"M0PL",  0xd008}, {"SIZEP0",0xd008}, {"M1PL",  0xd009}, {"SIZEP1",0xd009},
	{"M2PL",  0xd00a}, {"SIZEP2",0xd00a}, {"M3PL",  0xd00b}, {"SIZEP3",0xd00b},
	{"P0PL",  0xd00c}, {"SIZEM", 0xd00c}, {"P1PL",  0xd00d}, {"GRAFP0",0xd00d},
	{"P2PL",  0xd00e}, {"GRAFP1",0xd00e}, {"P3PL",  0xd00f}, {"GRAFP2",0xd00f},
	{"TRIG0", 0xd010}, {"GRAFP3",0xd010}, {"TRIG1", 0xd011}, {"GRAFM", 0xd011},
	{"TRIG2", 0xd012}, {"COLPM0",0xd012}, {"TRIG3", 0xd013}, {"COLPM1",0xd013},
	{"PAL",   0xd014}, {"COLPM2",0xd014}, {"COLPM3",0xd015}, {"COLPF0",0xd016},
	{"COLPF1",0xd017},
	{"COLPF2",0xd018}, {"COLPF3",0xd019}, {"COLBK", 0xd01a}, {"PRIOR", 0xd01b},
	{"VDELAY",0xd01c}, {"GRACTL",0xd01d}, {"HITCLR",0xd01e}, {"CONSOL",0xd01f},

	{"PBI",   0xd100}, /* PBI 1-254 */    {"PDVI",  0xd1ff}, {"PDVS",  0xd1ff},

	{"POT0",  0xd200}, {"AUDF1", 0xd200}, {"POT1",  0xd201}, {"AUDC1", 0xd201},
	{"POT2",  0xd202}, {"AUDF2", 0xd202}, {"POT3",  0xd203}, {"AUDC2", 0xd203},
	{"POT4",  0xd204}, {"AUDF3", 0xd204}, {"POT5",  0xd205}, {"AUDC3", 0xd205},
	{"POT6",  0xd206}, {"AUDF4", 0xd206}, {"POT7",  0xd207}, {"AUDC4", 0xd207},
	{"ALLPOT",0xd208}, {"AUDCTL",0xd208}, {"KBCODE",0xd209}, {"STIMER",0xd209},
	{"RANDOM",0xd20a}, {"SKRES", 0xd20a}, {"POTGO", 0xd20b},
	{"SERIN", 0xd20d}, {"SEROUT",0xd20d}, {"IRQST", 0xd20e}, {"IRQEN", 0xd20e},
	{"SKSTAT",0xd20f}, {"SKCTL", 0xd20f},

	{"PORTA", 0xd300}, {"PORTB", 0xd301}, {"PACTL", 0xd302}, {"PBCTL", 0xd303},

	{"DMACTL",0xd400}, {"CHACTL",0xd401}, {"DLISTL",0xd402}, {"DLISTH",0xd403},
	{"HSCROL",0xd404}, {"VSCROL",0xd405}, {"PMBASE",0xd407}, {"CHBASE",0xd409},
	{"WSYNC", 0xd40a}, {"VCOUNT",0xd40b}, {"PENH",  0xd40c}, {"PENV",  0xd40d},
	{"NMIEN", 0xd40e}, {"NMIST", 0xd40f}, {"NMIRES",0xd40f},

	{"PBIRAM",0xd600}, /* PMIRAM 1-255 */

	{"AFP",   0xd800},

	{"PDID1",   0xd803}, {"PDIOV", 0xd805}, {"PDIOV+1",0xd806},{"PDIRQV", 0xd808},
	{"PDIRQV+1",0xd809}, {"PDID2", 0xd80b}, {"PDVV", 0xd80d},

	{"FASC",  0xd8e6}, {"IFP",   0xd9aa}, {"FPI",   0xd9d2},
	{"ZFR0",  0xda44}, {"ZF1",   0xda46}, {"FSUB",  0xda60}, {"FADD",  0xda66},
	{"FMUL",  0xdadb}, {"FDIV",  0xdb28}, {"PLYEVL",0xdd40}, {"FLD0R", 0xdd89},
	{"FLD0P", 0xdd8d}, {"FLD1R", 0xdd98}, {"FLD1P", 0xdd9c}, {"FST0R", 0xdda7},
	{"FST0P", 0xddab}, {"FMOVE", 0xddb6}, {"EXP",   0xddc0}, {"EXP10", 0xddcc},
	{"LOG",   0xdecd}, {"LOG10", 0xded1},

	{"EDITRV",0xe400}, {"SCRENV",0xe410}, {"KEYBDV",0xe420}, {"PRINTV",0xe430},
	{"CASETV",0xe440},

	{"DINITV",0xe450}, {"DSKINV",0xe453}, {"CIOV",  0xe456}, {"SIOV",  0xe459},
	{"SETVBV",0xe45c}, {"SYSVBV",0xe45f}, {"XITVBV",0xe462}, {"SIOINV",0xe465},
	{"SENDEV",0xe468}, {"INTINV",0xe46b}, {"CIOINV",0xe46e}, {"BLKBDV",0xe471},
	{"WARMSV",0xe474}, {"COLDSV",0xe477}, {"RBLOKV",0xe47a}, {"CSOPIV",0xe47d},
	{"PUPDIV",0xe480}, {"SLFTSV",0xe483}, {"PHENTV",0xe486}, {"PHUNLV",0xe489},
	{"PHINIV",0xe48c}, {"GPDVV", 0xe48f},

	{NULL,    0x0000}
};

static const symtable_rec symtable_builtin_5200[] = {
	{"POKMSK",  0x0000}, {"RTCLOKH",  0x0001}, {"RTCLOKL",0x0002}, {"CRITIC",   0x0003},
	{"ATRACT", 0x0004}, {"SDLSTL",  0x0005}, {"SDLSTH",   0x0006}, {"SDMCTL",  0x0007},
	{"PCOLR0",    0x0008}, {"PCOLR1",  0x0009}, {"PCOLR2",0x000a}, {"PCOLR3",  0x000b},
	{"COLOR0",0x000c}, {"COLOR1",  0x000d}, {"COLOR2",0x000e}, {"COLOR3",  0x000f},
	{"COLOR4",  0x0010}, {"PADDL0",  0x0011}, {"PADDL1",0x0012}, {"PADDL2",0x0013},
	{"PADDL3",  0x0014}, {"PADDL4",0x0015}, {"PADDL5",  0x0016}, {"PADDL6",  0x0017},
	{"PADDL7",0x0018},

	{"VIMIRQ",  0x0200}, {"VIMIRQ+1",0x0201}, {"VVBLKI",  0x0202}, {"VVBLKI+1",0x0203},
	{"VVBLKD",  0x0204}, {"VVBLKD+1",0x0205}, {"VDSLST",  0x0206}, {"VDSLST+1",0x0207},
	{"VKEYBD",  0x0208}, {"VKEYBD+1",0x0209}, {"VKPD",  0x020a}, {"VKPD+1",0x020b},
	{"BRKKY",  0x020c}, {"BRKKY+1",0x020d}, {"VBREAK",  0x020e}, {"VBREAK+1",0x020f},
	{"VSERIN",  0x0210}, {"VSERIN+1",0x0211}, {"VSEROR",  0x0212}, {"VSEROR+1",0x0213},
	{"VSEROC",  0x0214}, {"VSEROC+1",0x0215}, {"VTIMR1",  0x0216}, {"VTIMR1+1",0x0217},
	{"VTIMR2",  0x0218}, {"VTIMR2+1",0x0219}, {"VTIMR4",  0x021a}, {"VTIMR4+1",0x021b},

	{"M0PF",  0xc000}, {"HPOSP0",0xc000}, {"M1PF",  0xc001}, {"HPOSP1",0xc001},
	{"M2PF",  0xc002}, {"HPOSP2",0xc002}, {"M3PF",  0xc003}, {"HPOSP3",0xc003},
	{"P0PF",  0xc004}, {"HPOSM0",0xc004}, {"P1PF",  0xc005}, {"HPOSM1",0xc005},
	{"P2PF",  0xc006}, {"HPOSM2",0xc006}, {"P3PF",  0xc007}, {"HPOSM3",0xc007},
	{"M0PL",  0xc008}, {"SIZEP0",0xc008}, {"M1PL",  0xc009}, {"SIZEP1",0xc009},
	{"M2PL",  0xc00a}, {"SIZEP2",0xc00a}, {"M3PL",  0xc00b}, {"SIZEP3",0xc00b},
	{"P0PL",  0xc00c}, {"SIZEM", 0xc00c}, {"P1PL",  0xc00d}, {"GRAFP0",0xc00d},
	{"P2PL",  0xc00e}, {"GRAFP1",0xc00e}, {"P3PL",  0xc00f}, {"GRAFP2",0xc00f},
	{"TRIG0", 0xc010}, {"GRAFP3",0xc010}, {"TRIG1", 0xc011}, {"GRAFM", 0xc011},
	{"TRIG2", 0xc012}, {"COLPM0",0xc012}, {"TRIG3", 0xc013}, {"COLPM1",0xc013},
	{"PAL",   0xc014}, {"COLPM2",0xc014}, {"COLPM3",0xc015}, {"COLPF0",0xc016},
	{"COLPF1",0xc017},
	{"COLPF2",0xc018}, {"COLPF3",0xc019}, {"COLBK", 0xc01a}, {"PRIOR", 0xc01b},
	{"VDELAY",0xc01c}, {"GRACTL",0xc01d}, {"HITCLR",0xc01e}, {"CONSOL",0xc01f},
	{"DMACTL",0xd400}, {"CHACTL",0xd401}, {"DLISTL",0xd402}, {"DLISTH",0xd403},
	{"HSCROL",0xd404}, {"VSCROL",0xd405}, {"PMBASE",0xd407}, {"CHBASE",0xd409},
	{"WSYNC", 0xd40a}, {"VCOUNT",0xd40b}, {"PENH",  0xd40c}, {"PENV",  0xd40d},
	{"NMIEN", 0xd40e}, {"NMIST", 0xd40f}, {"NMIRES",0xd40f},

	{"POT0",  0xe800}, {"AUDF1", 0xe800}, {"POT1",  0xe801}, {"AUDC1", 0xe801},
	{"POT2",  0xe802}, {"AUDF2", 0xe802}, {"POT3",  0xe803}, {"AUDC2", 0xe803},
	{"POT4",  0xe804}, {"AUDF3", 0xe804}, {"POT5",  0xe805}, {"AUDC3", 0xe805},
	{"POT6",  0xe806}, {"AUDF4", 0xe806}, {"POT7",  0xe807}, {"AUDC4", 0xe807},
	{"ALLPOT",0xe808}, {"AUDCTL",0xe808}, {"KBCODE",0xe809}, {"STIMER",0xe809},
	{"RANDOM",0xe80a}, {"SKRES", 0xe80a}, {"POTGO", 0xe80b},
	{"SERIN", 0xe80d}, {"SEROUT",0xe80d}, {"IRQST", 0xe80e}, {"IRQEN", 0xe80e},
	{"SKSTAT",0xe80f}, {"SKCTL", 0xe80f},

	{NULL,    0x0000}
};

static int symtable_builtin_enable = TRUE;

static symtable_rec *symtable_user = NULL;
static int symtable_user_size = 0;

#endif /* MONITOR_HINTS */

#ifdef MONITOR_ANSI
/* for color bitmaps: black, green, red, white for 00 01 10 11
	for mono, black & white for 0 and 1. */
static char *gr_color_chars[] = {
	"\x1b[30;40m ", "\x1b[30;42m ", "\x1b[30;41m ", "\x1b[30;47m " };
static char *gr_color_done = "\x1b[0m";
#else
static char *gr_color_chars[] = { " ", "*", "O", "X" };
static char *gr_color_done = "";
#endif


#ifdef MONITOR_UTF8
/* These are hard-coded UTF-8 encoded Unicode characters. Doing it this way
	has the advantage that the compiler (and the developer's text editor)
	doesn't have to have Unicode support, and the C library doesn't have to
	support wide characters. The disadvantage is that no other encodings
	besides UTF-8 can be supported (which shouldn't be a real problem these
	days). */
static const char *utf8_chars[] = {
/* 0 - 7 */ "\xe2\x99\xa5", "\xe2\x94\xa3", "\xe2\x94\x83", "\xe2\x94\x9b", "\xe2\x94\xab", "\xe2\x94\x93", "\xe2\x95\xb1", "\xe2\x95\xb2",
/* 8 - 15 */ "\xe2\x97\xa2", "\xe2\x96\x97", "\xe2\x97\xa3", "\xe2\x96\x9d", "\xe2\x96\x98", "\xe2\x96\x94", "\xe2\x96\x81", "\xe2\x96\x96",
/* 16 - 23 */ "\xe2\x99\xa3", "\xe2\x94\x8f", "\xe2\x94\x81", "\xe2\x95\x8b", "\xe2\x9a\xab", "\xe2\x96\x84", "\xe2\x96\x8e", "\xe2\x94\xb3",
/* 24 - 31 */ "\xe2\x94\xbb", "\xe2\x96\x8c", "\xe2\x94\x97", "\xe2\x90\x9b", "\xe2\x86\x91", "\xe2\x86\x93", "\xe2\x86\x90", "\xe2\x86\x92",
/* 32 - 39 */ " ", "!", "\"", "#", "$", "%", "&", "'",
/* 40 - 47 */ "(", ")", "*", "+", ",", "-", ".", "/",
/* 48 - 55 */ "0", "1", "2", "3", "4", "5", "6", "7",
/* 56 - 63 */ "8", "9", ":", ";", "<", "=", ">", "?",
/* 64 - 71 */ "@", "A", "B", "C", "D", "E", "F", "G",
/* 72 - 79 */ "H", "I", "J", "K", "L", "M", "N", "O",
/* 80 - 87 */ "P", "Q", "R", "S", "T", "U", "V", "W",
/* 88 - 95 */ "X", "Y", "Z", "[", "\\", "]", "^", "_",
/* 96 - 103 */ "\xe2\x97\x86", "a", "b", "c", "d", "e", "f", "g",
/* 104 - 111 */ "h", "i", "j", "k", "l", "m", "n", "o",
/* 112 - 119 */ "p", "q", "r", "s", "t", "u", "v", "w",
/* 120 - 127 */ "x", "y", "z", "\xe2\x99\xa0", "|", "\xe2\x86\xb0", "\xe2\x97\x80", "\xe2\x96\xb6",
};

/* Print an ATASCII character, with support for graphics characters if
	UTF-8 is available, and inverse video if ANSI is available. */
static void print_atascii_char(UWORD c) {
	int inv = c & 0x80;

#ifdef MONITOR_ANSI
	/* ESC[7m = reverse video attribute on */
	if(inv) printf("\x1b[7m");
#else
	if(inv) {
		putchar('.');
		return;
	}
#endif /* MONITOR_ANSI */

	printf("%s", utf8_chars[c & 0x7f]);

#ifdef MONITOR_ANSI
	/* ESC[0m = all attributes off */
	if(inv) printf("\x1b[0m");
#endif /* MONITOR_ANSI */
}
#else /* MONITOR_UTF8 */
static void print_atascii_char(UWORD c) {
	putchar((c >= ' ' && c <= 'z' && c != '\x60') ? c : '.');
}
#endif /* MONITOR_UTF8 */

#ifdef MONITOR_HINTS

static const char *find_label_name(UWORD addr, int is_write)
{
	int i;
	for (i = 0; i < symtable_user_size; i++) {
		if (symtable_user[i].addr == addr)
			return symtable_user[i].name;
	}
	if (symtable_builtin_enable) {
		const symtable_rec *p;
		for (p = (Atari800_machine_type == Atari800_MACHINE_5200 ? symtable_builtin_5200 : symtable_builtin); p->name != NULL; p++) {
			if (p->addr == addr) {
				if (is_write && p[1].addr == addr)
					p++;
				return p->name;
			}
		}
	}
	return NULL;
}

static symtable_rec *find_user_label(const char *name)
{
	int i;
	for (i = 0; i < symtable_user_size; i++) {
		if (Util_stricmp(symtable_user[i].name, name) == 0)
			return &symtable_user[i];
	}
	return NULL;
}

static int find_label_value(const char *name)
{
	const symtable_rec *p = find_user_label(name);
	if (p != NULL)
		return p->addr;
	if (symtable_builtin_enable) {
		for (p = (Atari800_machine_type == Atari800_MACHINE_5200 ? symtable_builtin_5200 : symtable_builtin); p->name != NULL; p++) {
			if (Util_stricmp(p->name, name) == 0)
				return p->addr;
		}
	}
	return -1;
}

static void free_user_labels(void)
{
	if (symtable_user != NULL) {
		while (symtable_user_size > 0)
			free(symtable_user[--symtable_user_size].name);
		free(symtable_user);
		symtable_user = NULL;
	}
}

static void add_user_label(const char *name, UWORD addr)
{
#define SYMTABLE_USER_INITIAL_SIZE 128
	if (symtable_user == NULL)
		symtable_user = (symtable_rec *) Util_malloc(SYMTABLE_USER_INITIAL_SIZE * sizeof(symtable_rec));
	else if (symtable_user_size >= SYMTABLE_USER_INITIAL_SIZE
	      && (symtable_user_size & (symtable_user_size - 1)) == 0) {
		/* symtable_user_size is a power of two: allocate twice as much */
		symtable_user = (symtable_rec *) Util_realloc(symtable_user,
			2 * symtable_user_size * sizeof(symtable_rec));
	}
	symtable_user[symtable_user_size].name = Util_strdup(name);
	symtable_user[symtable_user_size].addr = addr;
	symtable_user_size++;
}

static void load_user_labels(const char *filename)
{
	FILE *fp;
	char line[256];
	if (filename == NULL) {
		printf("You must specify a filename\n");
		return;
	}
	/* "rb" and not "r", because we strip EOLs ourselves
	   - this is better, because we can use CR/LF files on Unix */
	fp = fopen(filename, "rb");
	if (fp == NULL) {
		perror(filename);
		return;
	}
	free_user_labels();
	while (fgets(line, sizeof(line), fp)) {
		char *p;
		unsigned int value = 0;
		int digits = 0;
		/* Find first 4 hex digits or more. */
		/* We don't support "Cafe Assembler", "Dead Assembler" or "C0de Assembler". ;-) */
		for (p = line; *p != '\0'; p++) {
			if (*p >= '0' && *p <= '9') {
				value = (value << 4) + *p - '0';
				digits++;
			}
			else if (*p >= 'A' && *p <= 'F') {
				value = (value << 4) + *p - 'A' + 10;
				digits++;
			}
			else if (*p >= 'a' && *p <= 'f') {
				value = (value << 4) + *p - 'a' + 10;
				digits++;
			}
			else if (digits >= 4)
				break;
			else if (*p == '-')
				break; /* ignore labels with negative values */
			else {
				/* note that xasm can put "2" before the label value and mads puts "00" */
				value = 0;
				digits = 0;
			}
		}
		if (*p != ' ' && *p != '\t')
			continue;
		if (value > 0xffff || digits > 8)
			continue;
		do
			p++;
		while (*p == ' ' || *p == '\t');
		Util_chomp(p);
		if (*p == '\0')
			continue;
		add_user_label(p, (UWORD) value);
	}
	fclose(fp);
	printf("Loaded %d labels\n", symtable_user_size);
}

#endif /* MONITOR_HINTS */

static const char instr6502[256][10] = {
	"BRK", "ORA (1,X)", "CIM", "ASO (1,X)", "NOP 1", "ORA 1", "ASL 1", "ASO 1",
	"PHP", "ORA #1", "ASL", "ANC #1", "NOP 2", "ORA 2", "ASL 2", "ASO 2",

	"BPL 0", "ORA (1),Y", "CIM", "ASO (1),Y", "NOP 1,X", "ORA 1,X", "ASL 1,X", "ASO 1,X",
	"CLC", "ORA 2,Y", "NOP !", "ASO 2,Y", "NOP 2,X", "ORA 2,X", "ASL 2,X", "ASO 2,X",

	"JSR 2", "AND (1,X)", "CIM", "RLA (1,X)", "BIT 1", "AND 1", "ROL 1", "RLA 1",
	"PLP", "AND #1", "ROL", "ANC #1", "BIT 2", "AND 2", "ROL 2", "RLA 2",

	"BMI 0", "AND (1),Y", "CIM", "RLA (1),Y", "NOP 1,X", "AND 1,X", "ROL 1,X", "RLA 1,X",
	"SEC", "AND 2,Y", "NOP !", "RLA 2,Y", "NOP 2,X", "AND 2,X", "ROL 2,X", "RLA 2,X",


	"RTI", "EOR (1,X)", "CIM", "LSE (1,X)", "NOP 1", "EOR 1", "LSR 1", "LSE 1",
	"PHA", "EOR #1", "LSR", "ALR #1", "JMP 2", "EOR 2", "LSR 2", "LSE 2",

	"BVC 0", "EOR (1),Y", "CIM", "LSE (1),Y", "NOP 1,X", "EOR 1,X", "LSR 1,X", "LSE 1,X",
	"CLI", "EOR 2,Y", "NOP !", "LSE 2,Y", "NOP 2,X", "EOR 2,X", "LSR 2,X", "LSE 2,X",

	"RTS", "ADC (1,X)", "CIM", "RRA (1,X)", "NOP 1", "ADC 1", "ROR 1", "RRA 1",
	"PLA", "ADC #1", "ROR", "ARR #1", "JMP (2)", "ADC 2", "ROR 2", "RRA 2",

	"BVS 0", "ADC (1),Y", "CIM", "RRA (1),Y", "NOP 1,X", "ADC 1,X", "ROR 1,X", "RRA 1,X",
	"SEI", "ADC 2,Y", "NOP !", "RRA 2,Y", "NOP 2,X", "ADC 2,X", "ROR 2,X", "RRA 2,X",


	"NOP #1", "STA (1,X)", "NOP #1", "SAX (1,X)", "STY 1", "STA 1", "STX 1", "SAX 1",
	"DEY", "NOP #1", "TXA", "ANE #1", "STY 2", "STA 2", "STX 2", "SAX 2",

	"BCC 0", "STA (1),Y", "CIM", "SHA (1),Y", "STY 1,X", "STA 1,X", "STX 1,Y", "SAX 1,Y",
	"TYA", "STA 2,Y", "TXS", "SHS 2,Y", "SHY 2,X", "STA 2,X", "SHX 2,Y", "SHA 2,Y",

	"LDY #1", "LDA (1,X)", "LDX #1", "LAX (1,X)", "LDY 1", "LDA 1", "LDX 1", "LAX 1",
	"TAY", "LDA #1", "TAX", "ANX #1", "LDY 2", "LDA 2", "LDX 2", "LAX 2",

	"BCS 0", "LDA (1),Y", "CIM", "LAX (1),Y", "LDY 1,X", "LDA 1,X", "LDX 1,Y", "LAX 1,X",
	"CLV", "LDA 2,Y", "TSX", "LAS 2,Y", "LDY 2,X", "LDA 2,X", "LDX 2,Y", "LAX 2,Y",


	"CPY #1", "CMP (1,X)", "NOP #1", "DCM (1,X)", "CPY 1", "CMP 1", "DEC 1", "DCM 1",
	"INY", "CMP #1", "DEX", "SBX #1", "CPY 2", "CMP 2", "DEC 2", "DCM 2",

	"BNE 0", "CMP (1),Y", "ESCRTS #1", "DCM (1),Y", "NOP 1,X", "CMP 1,X", "DEC 1,X", "DCM 1,X",
	"CLD", "CMP 2,Y", "NOP !", "DCM 2,Y", "NOP 2,X", "CMP 2,X", "DEC 2,X", "DCM 2,X",


	"CPX #1", "SBC (1,X)", "NOP #1", "INS (1,X)", "CPX 1", "SBC 1", "INC 1", "INS 1",
	"INX", "SBC #1", "NOP", "SBC #1 !", "CPX 2", "SBC 2", "INC 2", "INS 2",

	"BEQ 0", "SBC (1),Y", "ESCAPE #1", "INS (1),Y", "NOP 1,X", "SBC 1,X", "INC 1,X", "INS 1,X",
	"SED", "SBC 2,Y", "NOP !", "INS 2,Y", "NOP 2,X", "SBC 2,X", "INC 2,X", "INS 2,X"
};

/* Opcode type:
   bits 1-0 = instruction length
   bit 2    = instruction reads from memory (without stack-manipulating instructions)
   bit 3    = instruction writes to memory (without stack-manipulating instructions)
   bits 7-4 = adressing type:
     0 = NONE (implicit)
     1 = ABSOLUTE
     2 = ZPAGE
     3 = ABSOLUTE_X
     4 = ABSOLUTE_Y
     5 = INDIRECT_X
     6 = INDIRECT_Y
     7 = ZPAGE_X
     8 = ZPAGE_Y
     9 = RELATIVE
     A = IMMEDIATE
     B = STACK 2 (RTS)
     C = STACK 3 (RTI)
     D = INDIRECT (JMP () )
     E = ESCRTS
     F = ESCAPE */
const UBYTE MONITOR_optype6502[256] = {
	0x01, 0x56, 0x01, 0x5e, 0x22, 0x26, 0x2e, 0x2e, 0x01, 0xa2, 0x01, 0xa2, 0x13, 0x17, 0x1f, 0x1f,
	0x92, 0x66, 0x01, 0x6e, 0x72, 0x76, 0x7e, 0x7e, 0x01, 0x47, 0x01, 0x4f, 0x33, 0x37, 0x3f, 0x3f,
	0x13, 0x56, 0x01, 0x5e, 0x26, 0x26, 0x2e, 0x2e, 0x01, 0xa2, 0x01, 0xa2, 0x17, 0x17, 0x1f, 0x1f,
	0x92, 0x66, 0x01, 0x6e, 0x72, 0x76, 0x7e, 0x7e, 0x01, 0x47, 0x01, 0x4f, 0x33, 0x37, 0x3f, 0x3f,
	0xc1, 0x56, 0x01, 0x5e, 0x22, 0x26, 0x2e, 0x2e, 0x01, 0xa2, 0x01, 0xa2, 0x13, 0x17, 0x1f, 0x1f,
	0x92, 0x66, 0x01, 0x6e, 0x72, 0x76, 0x7e, 0x7e, 0x01, 0x47, 0x01, 0x4f, 0x33, 0x37, 0x3f, 0x3f,
	0xb1, 0x56, 0x01, 0x5e, 0x22, 0x26, 0x2e, 0x2e, 0x01, 0xa2, 0x01, 0xa2, 0xd3, 0x17, 0x1f, 0x1f,
	0x92, 0x66, 0x01, 0x6e, 0x72, 0x76, 0x7e, 0x7e, 0x01, 0x47, 0x01, 0x4f, 0x33, 0x37, 0x3f, 0x3f,
	0xa2, 0x5a, 0x01, 0x5a, 0x2a, 0x2a, 0x2a, 0x2a, 0x01, 0xa2, 0x01, 0xa2, 0x1b, 0x1b, 0x1b, 0x1b,
	0x92, 0x6a, 0x01, 0x6a, 0x7a, 0x7a, 0x8a, 0x8a, 0x01, 0x4b, 0x01, 0x4b, 0x3b, 0x3b, 0x4b, 0x4b,
	0xa2, 0x56, 0xa2, 0x56, 0x26, 0x26, 0x26, 0x26, 0x01, 0xa2, 0x01, 0xa2, 0x17, 0x17, 0x17, 0x17,
	0x92, 0x66, 0x01, 0x66, 0x76, 0x76, 0x86, 0x86, 0x01, 0x47, 0x01, 0x47, 0x37, 0x37, 0x47, 0x47,
	0xa2, 0x56, 0xa2, 0x5e, 0x26, 0x26, 0x2e, 0x2e, 0x01, 0xa2, 0x01, 0xa2, 0x17, 0x17, 0x1f, 0x1f,
	0x92, 0x66, 0xe2, 0x6e, 0x72, 0x76, 0x7e, 0x7e, 0x01, 0x47, 0x01, 0x4f, 0x33, 0x37, 0x3f, 0x3f,
	0xa2, 0x56, 0xa2, 0x5e, 0x26, 0x26, 0x2e, 0x2e, 0x01, 0xa2, 0x01, 0xa2, 0x17, 0x17, 0x1f, 0x1f,
	0x92, 0x66, 0xf2, 0x6e, 0x72, 0x76, 0x7e, 0x7e, 0x01, 0x47, 0x01, 0x4f, 0x33, 0x37, 0x3f, 0x3f
};


static void safe_gets(char *buffer, size_t size, char const *prompt)
{
#ifdef HAVE_FFLUSH
	fflush(stdout);
#endif

#ifdef MONITOR_READLINE
	{
		char *got = readline(prompt);
		if (got) {
			strncpy(buffer, got, size);
			buffer[size-1]='\0';
			if (*got)
				add_history(got);
			free(got); /* Need to free buffer allocated by readline() */
		}
	}
#else
/// TODO:
///	fputs(prompt, stdout);
///	if (fgets(buffer, size, stdin) == NULL)
		buffer[0] = 0;
#endif
	Util_chomp(buffer);
}

static int pager(void)
{
	char buf[100];
	safe_gets(buf, sizeof(buf), "Press Return to continue ('q' to quit): ");
	return buf[0] == 'q' || buf[0] == 'Q';
}

static char *token_ptr;

static char *get_token(void)
{
	char *p = token_ptr;
	while (*p == ' ')
		p++;
	if (*p == '\0')
		return NULL;
	token_ptr = p;
	do {
		token_ptr++;
		if (*token_ptr == ' ') {
			*token_ptr++ = '\0';
			break;
		}
	} while (*token_ptr != '\0');
	return p;
}

static int get_dec(int *decval)
{
	const char *t;
	t = get_token();
	if (t != NULL) {
		int x = Util_sscandec(t);
		if (x < 0)
			return FALSE;
		*decval = x;
		return TRUE;
	}
	return FALSE;
}

/* Parses S in search for a hexadecimal number. On success stores the number
   in HEXVAL and returns TRUE; otherwise returns FALSE. */
static int real_parse_hex(const char *s, UWORD *hexval)
{
	int x = Util_sscanhex(s);
#ifdef MONITOR_HINTS
	int y = find_label_value(s);
	if (y >= 0) {
		if (x < 0 || x > 0xffff || x == y) {
			*hexval = (UWORD) y;
			return TRUE;
		}
		/* s can be a hex number or a label name */
		printf("%s is ambiguous. Use 0%X or %X instead.\n", s, x, y);
		return FALSE;
	}
#endif
	if (x < 0 || x > 0xffff)
		return FALSE;
	*hexval = (UWORD) x;
	return TRUE;
}

/* Parse s, support * (byte deference) and @ (word deref). Recursive calls
	support stacking (e.g. **label, *@label) */
static int parse_hex(const char *s, UWORD *hexval)
{
	UWORD addr;
	char deref_type = '\0';

	switch(s[0]) {
		case '\0': return FALSE; /* don't operate on empty string */
		case '*':
		case '@':
			deref_type = s[0];
			s++;
			if(!parse_hex(s, &addr))
				return FALSE;
			break;
		default:
			if(!real_parse_hex(s, &addr))
				return FALSE;
			break;
	}


	switch(deref_type) {
		case '\0':
			*hexval = addr;
			break;
		case '*':
			*hexval = MEMORY_SafeGetByte(addr);
			break;
		case '@':
			*hexval = MEMORY_SafeGetByte(addr) | (MEMORY_SafeGetByte(addr + 1) << 8);
			break;
		default:
			return FALSE; /* should never happen */
	}

	return TRUE;
}

/* Searches for a hexadecimal number in the command line. On success stores
   the number in HEXVAL and returns TRUE; otherwise returns FALSE. */
static int get_hex(UWORD *hexval)
{
	const char *t;
	t = get_token();
	if (t != NULL)
		return parse_hex(t, hexval);
	return FALSE;
}

static int get_hex2(UWORD *hexval1, UWORD *hexval2)
{
	return get_hex(hexval1) && get_hex(hexval2);
}

static int get_hex3(UWORD *hexval1, UWORD *hexval2, UWORD *hexval3)
{
	return get_hex(hexval1) && get_hex(hexval2) && get_hex(hexval3);
}

static void get_uword(UWORD *val)
{
	if (!get_hex(val))
		printf("Invalid argument!\n");
}

static void get_ubyte(UBYTE *val)
{
	UWORD uword;
	if (!get_hex(&uword) || uword > 0xff) {
		printf("Invalid argument!\n");
	} else
		*val = (UBYTE) uword;
}

static int get_bool(void)
{
	const char *t;
	t = get_token();
	if (t != NULL) {
		int result = Util_sscanbool(t);
		if (result >= 0)
			return result;
	}
	printf("Invalid argument (should be 0 or 1)!\n");
	return -1;
}

static int get_attrib_range(UWORD *addr1, UWORD *addr2)
{
	if (get_hex2(addr1, addr2) && *addr1 <= *addr2) {
#ifdef PAGED_ATTRIB
		if ((*addr1 & 0xff) != 0 || (*addr2 & 0xff) != 0xff) {
			printf("This is PAGED_ATTRIB version of Atari800.\n"
			       "You can only change attributes of full memory pages.\n");
			return FALSE;
		}
#endif
		return TRUE;
	}
	printf("Missing or bad argument!\n");
	return FALSE;
}

static UWORD show_instruction(FIL *fp, UWORD pc)
{
	UWORD addr = pc;
	UBYTE insn;
	const char *mnemonic;
	const char *p;
	int value = 0;
	int nchars = 0;

	insn = MEMORY_SafeGetByte(pc);
	pc++;
	mnemonic = instr6502[insn];
	for (p = mnemonic + 3; *p != '\0'; p++) {
		if (*p == '1') {
			value = MEMORY_SafeGetByte(pc);
			pc++;
			fprintf(fp, nchars, "%04X: %02X %02X     " /*"%Xcyc  "*/ "%.*s$%02X%s",
			                 addr, insn, value, /*cycles[insn],*/ (int) (p - mnemonic), mnemonic, value, p + 1);
			break;
		}
		if (*p == '2') {
			value = MEMORY_SafeGetByte(pc) + (MEMORY_SafeGetByte(pc + 1) << 8);
			fprintf(fp, nchars, "%04X: %02X %02X %02X  " /*"%Xcyc  "*/ "%.*s$%04X%s",
			                 addr, insn, value & 0xff, value >> 8, /*cycles[insn],*/ (int) (p - mnemonic), mnemonic, value, p + 1);
			pc += 2;
			break;
		}
		if (*p == '0') {
			UBYTE op = MEMORY_SafeGetByte(pc);
			pc++;
			value = (UWORD) (pc + (SBYTE) op);
			fprintf(fp, nchars, "%04X: %02X %02X     " /*"3cyc  "*/ "%.4s$%04X", addr, insn, op, mnemonic, value);
			break;
		}
	}
	if (*p == '\0') {
		fprintf(fp, "%04X: %02X        " /*"%Xcyc  "*/ "%s\n", addr, insn, /*cycles[insn],*/ mnemonic);
		return pc;
	}
#ifdef MONITOR_HINTS
	if (p[-1] != '#') {
		/* different names when reading/writing memory */
		const char *label = find_label_name((UWORD) value, (MONITOR_optype6502[insn] & 0x08) != 0);
		if (label != NULL) {
			fprintf(fp, "%*s;%s\n", 28 - nchars, "", label);
			return pc;
		}
	}
#else
	(void)nchars;
#endif
	fputc('\n', fp);
	return pc;
}

void MONITOR_Exit(void)
{
	if (trainer_memory != NULL) {
		free(trainer_memory);
		trainer_memory=NULL;
		trainer_flags=NULL;
	}
}

void MONITOR_ShowState(FIL *fp, UWORD pc, UBYTE a, UBYTE x, UBYTE y, UBYTE s,
                char n, char v, char z, char c)
{
	fprintf(fp, "%3d %3d A=%02X X=%02X Y=%02X S=%02X P=%c%c*-%c%c%c%c PC=",
		ANTIC_ypos, ANTIC_XPOS, a, x, y, s,
		n, v, (CPU_regP & CPU_D_FLAG) ? 'D' : '-', (CPU_regP & CPU_I_FLAG) ? 'I' : '-', z, c);
	show_instruction(fp, pc);
}

static void show_state(void)
{
	MONITOR_ShowState(stdout, CPU_regPC, CPU_regA, CPU_regX, CPU_regY, CPU_regS,
		(char) ((CPU_regP & CPU_N_FLAG) ? 'N' : '-'), (char) ((CPU_regP & CPU_V_FLAG) ? 'V' : '-'),
		(char) ((CPU_regP & CPU_Z_FLAG) ? 'Z' : '-'), (char) ((CPU_regP & CPU_C_FLAG) ? 'C' : '-'));
}

static UWORD disassemble(UWORD addr)
{
	int count = 24;
	do
		addr = show_instruction(stdout, addr);
	while (--count > 0);
	return addr;
}

#ifdef MONITOR_ASSEMBLER
static UWORD assembler(UWORD addr)
{
	printf("Simple assembler (enter empty line to exit)\n");
	for (;;) {
		char s[128];  /* input string */
		char c[128];  /* converted input */
		char *sp;     /* input pointer */
		char *cp;     /* converted input pointer */
		char *vp;     /* value pointer (the value is stored in s) */
		char *tp;     /* type pointer (points at type character '0', '1' or '2' in converted input) */
		int i;
		int isa;      /* the operand is "A" */
		UWORD value = 0;

		char prompt[7];
		snprintf(prompt, sizeof(prompt), "%04X: ", (int) addr);
		safe_gets(s, sizeof(s), prompt);
		if (s[0] == '\0')
			return addr;

		Util_strupper(s);

		sp = s;
		cp = c;
		/* copy first three characters */
		for (i = 0; i < 3 && *sp != '\0'; i++)
			*cp++ = *sp++;
		/* insert space before operands */
		*cp++ = ' ';

		tp = NULL;
		isa = FALSE;

		/* convert input to format of instr6502[] table */
		while (*sp != '\0') {
			switch (*sp) {
			case ' ':
			case '\t':
			case '$':
			case '@':
				sp++;
				break;
			case '#':
			case '(':
			case ')':
			case ',':
				isa = FALSE;
				*cp++ = *sp++;
				break;
			default:
				if (tp != NULL) {
					if (*sp == 'X' || *sp == 'Y') {
						*cp++ = *sp++;
						break;
					}
					goto invalid_instr;
				}
				vp = s;
				do
					*vp++ = *sp++;
				while (strchr(" \t$@#(),", *sp) == NULL && *sp != '\0');
				/* If *sp=='\0', strchr() should return non-NULL,
				   but we do an extra check to be on safe side. */
				*vp++ = '\0';
				tp = cp++;
				*tp = '0';
				isa = (s[0] == 'A' && s[1] == '\0');
				break;
			}
		}
		if (cp[-1] == ' ')
			cp--;    /* no arguments (e.g. NOP or ASL @) */
		*cp = '\0';

		/* if there's an operand, get its value */
		if (tp != NULL && !parse_hex(s, &value)) {
			printf("Invalid operand!\n");
			continue;
		}

		for (;;) {
			/* search table for instruction */
			for (i = 0; i < 256; i++) {
				if (strcmp(instr6502[i], c) == 0) {
					if (tp == NULL) {
						MEMORY_dPutByte(addr, (UBYTE) i);
						addr++;
					}
					else if (*tp == '0') {
						value -= (addr + 2);
						if ((SWORD) value < -128 || (SWORD) value > 127)
							printf("Branch out of range!\n");
						else {
							MEMORY_dPutByte(addr, (UBYTE) i);
							addr++;
							MEMORY_dPutByte(addr, (UBYTE) value);
							addr++;
						}
					}
					else if (*tp == '1') {
						c[3] = '\0';
						if (isa && (strcmp(c, "ASL") == 0 || strcmp(c, "LSR") == 0 ||
						            strcmp(c, "ROL") == 0 || strcmp(c, "ROR") == 0)) {
							printf("\"%s A\" is ambiguous.\n"
							       "Use \"%s\" for accumulator mode or \"%s 0A\" for zeropage mode.\n", c, c, c);
						}
						else {
							MEMORY_dPutByte(addr, (UBYTE) i);
							addr++;
							MEMORY_dPutByte(addr, (UBYTE) value);
							addr++;
						}
					}
					else { /* *tp == '2' */
						MEMORY_dPutByte(addr, (UBYTE) i);
						addr++;
						MEMORY_dPutWord(addr, value);
						addr += 2;
					}
					goto next_instr;
				}
			}
			/* not found */
			if (tp == NULL || *tp == '2')
				break;
			if (++*tp == '1' && value > 0xff)
				*tp = '2';
		}
	invalid_instr:
		printf("Invalid instruction!\n");
	next_instr:
		;
	}
}
#endif /* MONITOR_ASSEMBLER */

#ifdef MONITOR_PROFILE
MONITOR_coverage_rec MONITOR_coverage[0x10000];
unsigned long MONITOR_coverage_insns;
unsigned long MONITOR_coverage_cycles;
#endif /* MONITOR_PROFILE */

#ifdef MONITOR_BREAK
UWORD MONITOR_break_addr = 0xd000;
UBYTE MONITOR_break_step = FALSE;
static UBYTE break_over = FALSE;
UBYTE MONITOR_break_ret = FALSE;
UBYTE MONITOR_break_brk = FALSE;
int MONITOR_ret_nesting = 0;
#endif

#ifdef MONITOR_BREAKPOINTS

MONITOR_breakpoint_cond MONITOR_breakpoint_table[MONITOR_BREAKPOINT_TABLE_MAX];
int MONITOR_breakpoint_table_size = 0;
int MONITOR_breakpoints_enabled = TRUE;

static void breakpoint_print_flag(int flagmask)
{
	switch (flagmask) {
	case CPU_N_FLAG:
		putchar('N');
		break;
	case CPU_V_FLAG:
		putchar('V');
		break;
	case CPU_D_FLAG:
		putchar('D');
		break;
	case CPU_I_FLAG:
		putchar('I');
		break;
	case CPU_Z_FLAG:
		putchar('Z');
		break;
	case CPU_C_FLAG:
		putchar('C');
		break;
	}
}

static int breakpoint_scan_flag(char c)
{
	switch (c) {
	case 'N':
		return CPU_N_FLAG;
	case 'V':
		return CPU_V_FLAG;
	case 'D':
		return CPU_D_FLAG;
	case 'I':
		return CPU_I_FLAG;
	case 'Z':
		return CPU_Z_FLAG;
	case 'C':
		return CPU_C_FLAG;
	default:
		return -1;
	}
}

static void breakpoints_set(int enabled)
{
	int i;
	if (get_dec(&i)) {
		do {
			if (/*i >= 0 &&*/ i < MONITOR_breakpoint_table_size)
				MONITOR_breakpoint_table[i].enabled = (UBYTE) enabled;
		} while (get_dec(&i));
	}
	else
		MONITOR_breakpoints_enabled = enabled;
}

static void monitor_breakpoints(void)
{
	char *t = get_token();
	if (t == NULL) {
		int i;
		if (MONITOR_breakpoint_table_size == 0) {
			printf("No breakpoints defined\n");
			return;
		}
		printf("Breakpoints are %sabled\n", MONITOR_breakpoints_enabled ? "en" : "dis");
		for (i = 0; i < MONITOR_breakpoint_table_size; i++) {
			printf("%2d: ", i);
			if (!MONITOR_breakpoint_table[i].enabled)
				printf("OFF ");
			switch (MONITOR_breakpoint_table[i].condition) {
			case MONITOR_BREAKPOINT_OR:
				printf("OR");
				break;
			case MONITOR_BREAKPOINT_FLAG_CLEAR:
				printf("CLR");
				breakpoint_print_flag(MONITOR_breakpoint_table[i].value);
				break;
			case MONITOR_BREAKPOINT_FLAG_SET:
				printf("SET");
				breakpoint_print_flag(MONITOR_breakpoint_table[i].value);
				break;
			default:
				{
					const char *op;
					switch (MONITOR_breakpoint_table[i].condition & 7) {
					case MONITOR_BREAKPOINT_LESS:
						op = "<";
						break;
					case MONITOR_BREAKPOINT_EQUAL:
						op = "=";
						break;
					case MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL:
						op = "<=";
						break;
					case MONITOR_BREAKPOINT_GREATER:
						op = ">";
						break;
					case MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL:
						op = ">=";
						break;
					case MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER:
						op = "!=";
						break;
					default:
						op = "?";
						break;
					}
					switch (MONITOR_breakpoint_table[i].condition >> 3) {
					case MONITOR_BREAKPOINT_PC >> 3:
						printf("PC%s%04X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_A >> 3:
						printf("A%s%02X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_X >> 3:
						printf("X%s%02X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_Y >> 3:
						printf("Y%s%02X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_S >> 3:
						printf("S%s%02X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_READ >> 3:
						printf("READ%s%04X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_WRITE >> 3:
						printf("WRITE%s%04X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_ACCESS >> 3:
						printf("ACCESS%s%04X", op, MONITOR_breakpoint_table[i].value);
						break;
					case MONITOR_BREAKPOINT_MEMORY >> 3:
						printf("MEM:%04x%s%04X", MONITOR_breakpoint_table[i].m_addr, op, MONITOR_breakpoint_table[i].value);
						break;
					default:
						printf("???");
						break;
					}
				}
			}
			putchar('\n');
		}
		return;
	}
	if (strcmp(t, "?") == 0) {
		printf(
			"B                - print breakpoint table\n"
			"B ?              - this help\n"
			"B C              - clear breakpoint table\n"
			"B D pos          - delete one entry\n"
			"B ON             - enable breakpoints\n"
			"B OFF            - disable breakpoints\n"
			"B ON pos1...     - enable specified breakpoints\n"
			"B OFF pos1...    - disable specified breakpoints\n"
			"B [pos] cond1... - insert breakpoints (at the specified position)\n"
			"    cond is: TYPE OPERATOR VALUE (without spaces)\n"
			"         or: SETFLAG, CLRFLAG where FLAG is: N, V, D, I, Z, C\n");
		printf(
			"    TYPE is: PC, A, X, Y, S, READ, WRITE, ACCESS (read or write),\n"
			"             MEM:addr (contents of memory)\n"
			"OPERATOR is: <, <=, =, ==, >, >=, !=, <>\n"
			"   VALUE is a hex number\n");
		printf(
			"Breakpoint conditions are connected by AND operator\n"
			"unless you explicitly use OR.\n"
			"Examples:\n"
			"B PC>=203f A<3a OR PC=3a7f X<>0 - creates 5 new entries\n"
			"B 2 Y<5                         - adds a new entry at position 2\n"
			"B D 1                           - deletes the entry at position 1\n"
			"B OR SETD                       - appends 2 new entries\n");
		return;
	}
	Util_strupper(t);
	if (strcmp(t, "C") == 0) {
		MONITOR_breakpoint_table_size = 0;
		printf("Breakpoint table cleared\n");
	}
	else if (strcmp(t, "D") == 0) {
		int i;
		if (get_dec(&i) && /*i >= 0 &&*/ i < MONITOR_breakpoint_table_size) {
			MONITOR_breakpoint_table_size--;
			while (i < MONITOR_breakpoint_table_size) {
				MONITOR_breakpoint_table[i] = MONITOR_breakpoint_table[i + 1];
				i++;
			}
			printf("Entry deleted\n");
		}
		else
			printf("Missing or bad argument\n");
	}
	else if (strcmp(t, "ON") == 0) {
		breakpoints_set(TRUE);
	}
	else if (strcmp(t, "OFF") == 0) {
		breakpoints_set(FALSE);
	}
	else {
		int i;
		if (t[0] >= '0' && t[0] <= '9') {
			i = Util_sscandec(t);
			if (i < 0 || i > MONITOR_breakpoint_table_size) {
				printf("Bad argument\n");
				return;
			}
			t = get_token();
			if (t == NULL) {
				printf("Missing arguments\n");
				return;
			}
		}
		else
			i = MONITOR_breakpoint_table_size;
		while (MONITOR_breakpoint_table_size < MONITOR_BREAKPOINT_TABLE_MAX) {
			UWORD condition;
			int value, m_addr = -1;
			int j;
			if (strcmp(t, "OR") == 0) {
				condition = MONITOR_BREAKPOINT_OR;
				value = 0;
			}
			else if (strncmp(t, "CLR", 3) == 0) {
				condition = MONITOR_BREAKPOINT_FLAG_CLEAR;
				value = breakpoint_scan_flag(t[3]);
			}
			else if (strncmp(t, "SET", 3) == 0) {
				condition = MONITOR_BREAKPOINT_FLAG_SET;
				value = breakpoint_scan_flag(t[3]);
			}
			else {
				condition = 0;
				switch (t[0]) {
				case 'A':
					if (strncmp(t, "ACCESS", 6) == 0) {
						condition = MONITOR_BREAKPOINT_ACCESS;
						t += 6;
					}
					else {
						condition = MONITOR_BREAKPOINT_A;
						t++;
					}
					break;
				case 'X':
					condition = MONITOR_BREAKPOINT_X;
					t++;
					break;
				case 'Y':
					condition = MONITOR_BREAKPOINT_Y;
					t++;
					break;
				case 'P':
					if (t[1] == 'C') {
						condition = MONITOR_BREAKPOINT_PC;
						t += 2;
					}
					break;
				case 'R':
					if (strncmp(t, "READ", 4) == 0) {
						condition = MONITOR_BREAKPOINT_READ;
						t += 4;
					}
					break;
				case 'S':
					condition = MONITOR_BREAKPOINT_S;
					t++;
					break;
				case 'W':
					if (strncmp(t, "WRITE", 5) == 0) {
						condition = MONITOR_BREAKPOINT_WRITE;
						t += 5;
					}
					break;
				case 'M':
					if (strncmp(t, "MEM:", 4) == 0) {
						UWORD tmp, i;
						char *p;
						char b[100];
						condition = MONITOR_BREAKPOINT_MEMORY;
						t += 4;
						p = t;
						for(i = 0; i < 100; i++) {
							if(
									t[i] == ' ' || t[i] == '!' ||
									t[i] == '<' || t[i] == '>' ||
									t[i] == '=' || t[i] == '\0')
							{
								b[i] = '\0';
								break;
							}
							b[i] = t[i];
							p++;
						}
						if (parse_hex(b, &tmp))
							m_addr = tmp;
						t = p;
					}
					break;
				default:
					break;
				}
				if(condition == MONITOR_BREAKPOINT_MEMORY && (m_addr < 0 || m_addr > 0xffff)) {
					printf("Bad address for MEM:\n");
					return;
				}
				if (t[0] == '!' && t[1] == '=') {
					condition += MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER;
					t += 2;
				}
				else {
					if (*t == '<') {
						condition += MONITOR_BREAKPOINT_LESS;
						t++;
					}
					if (*t == '>') {
						condition += MONITOR_BREAKPOINT_GREATER;
						t++;
					}
					if (*t == '=') {
						condition += MONITOR_BREAKPOINT_EQUAL;
						t++;
						if (*t == '=')
							t++;
					}
				}
				if ((condition >> 3) == 0 || (condition & 7) == 0) {
					printf("Bad argument\n");
					return;
				}
				{
					UWORD tmp;
					if (parse_hex(t, &tmp))
						value = tmp;
					else
						value = -1;
				}
			}
			if (value < 0 || value > 0xffff) {
				printf("Bad argument\n");
				return;
			}
			for (j = MONITOR_breakpoint_table_size; j > i; j--)
				MONITOR_breakpoint_table[j] = MONITOR_breakpoint_table[j - 1];
			MONITOR_breakpoint_table[i].enabled = TRUE;
			MONITOR_breakpoint_table[i].condition = condition;
			MONITOR_breakpoint_table[i].value = (UWORD) value;
			MONITOR_breakpoint_table[i].m_addr = (UWORD) m_addr;
			i++;
			MONITOR_breakpoint_table_size++;
			t = get_token();
			if (t == NULL) {
				printf("Breakpoint(s) added\n");
				return;
			}
			Util_strupper(t);
		}
		printf("Breakpoint table full\n");
	}
}

#endif /* MONITOR_BREAKPOINTS */

#ifdef MONITOR_BREAK
/* Enables/disables breakpoints on BRK according to the command line. */
static void monitor_break_BRK(void)
{
	char *t = get_token();
	if (t == NULL)
		printf("Break on BRK is %sabled\n", MONITOR_break_brk ? "en" : "dis");
	else if (Util_stricmp(t, "ON") == 0)
		MONITOR_break_brk = TRUE;
	else if (Util_stricmp(t, "OFF") == 0)
		MONITOR_break_brk = FALSE;
	else
		printf("Invalid argument. Usage: BBRK ON or OFF\n");
}

/* Enables/disables breakpoint in a specific program counter value, fetched
   from command line. */
static void monitor_break_PC(void)
{
	int addr_valid = get_hex(&MONITOR_break_addr);
	if (addr_valid)
	{
		if (MONITOR_break_addr >= 0xd000 && MONITOR_break_addr <= 0xd7ff)
			printf("PC breakpoint disabled\n");
		else
			printf("Breakpoint set at PC=%04X\n", MONITOR_break_addr);
	}
	else
	{
		printf("Breakpoint is at PC=%04X\n", MONITOR_break_addr);
	}
}

/* Displays last 64 executed instructions. */
static void show_history(void)
{
	int i;
	for (i = 0; i < CPU_REMEMBER_PC_STEPS; i++) {
		int j;
		int k;
		UWORD saved_cpu = CPU_remember_PC[(CPU_remember_PC_curpos + i) % CPU_REMEMBER_PC_STEPS];
		UBYTE save_op[3];
		j = CPU_remember_xpos[(CPU_remember_PC_curpos + i) % CPU_REMEMBER_PC_STEPS];
		printf("%3d %3d ", j >> 8, j & 0xff);
		for (k = 0; k < 3; k++) {
			save_op[k] = MEMORY_SafeGetByte(saved_cpu + k);
			MEMORY_dPutByte(saved_cpu + k, CPU_remember_op[(CPU_remember_PC_curpos + i) % CPU_REMEMBER_PC_STEPS][k]);
		}
		show_instruction(stdout, CPU_remember_PC[(CPU_remember_PC_curpos + i) % CPU_REMEMBER_PC_STEPS]);
		for (k = 0; k < 3; k++) {
			MEMORY_dPutByte(saved_cpu + k, save_op[k]);
		}
	}
}

/* Displays last 16 executed JMP/JSR instructions. */
static void show_last_jumps(void)
{
	int i;
	for (i = 0; i < CPU_REMEMBER_JMP_STEPS; i++)
		show_instruction(stdout, CPU_remember_JMP[(CPU_remember_jmp_curpos + i) % CPU_REMEMBER_JMP_STEPS]);
}

/* Stesp over the current instruction. */
static void step_over(void)
{
	UBYTE opcode = MEMORY_SafeGetByte(CPU_regPC);
	if ((opcode & 0x1f) == 0x10 || opcode == 0x20) {
		/* branch or JSR: set breakpoint after it */
		MONITOR_break_addr = CPU_regPC + (MONITOR_optype6502[MEMORY_SafeGetByte(CPU_regPC)] & 0x3);
		break_over = TRUE;
	}
	else
		MONITOR_break_step = TRUE;
}
#endif /* MONITOR_BREAK */

#if defined(MONITOR_BREAK) || !defined(NO_YPOS_BREAK_FLICKER)
/* Enables/disables breakpoint or blinking of a scanline specified at command
   line. */
static void monitor_bline(void)
{
	get_dec(&ANTIC_break_ypos);
	if (ANTIC_break_ypos >= 1008 && ANTIC_break_ypos <= 1247) {
		printf("Blinking scanline %d\n", ANTIC_break_ypos - 1000);
#ifdef MONITOR_BREAK
	else if (ANTIC_break_ypos >= 0 && ANTIC_break_ypos <= 311)
		printf("Breakpoint set at scanline %d\n", ANTIC_break_ypos);
#endif
	} else {
		printf("BLINE disabled\n");
	}
}
#endif

/* Displays ANTIC's Display List. */
static void show_dlist(void)
{
#if 0
	/* one instruction per line */
	UWORD tdlist = dlist;
	int nlines = 0;
	get_hex(&tdlist);
	for (;;) {
		UBYTE IR;

		printf("%04X: ", tdlist);
		IR = ANTIC_GetDLByte(&tdlist);

		if (IR & 0x80)
		printf("DLI ");

		if ((IR & 0x0f) == 0)
		printf("%c BLANK\n", ((IR >> 4) & 0x07) + '1');
		else if ((IR & 0x0f) == 1) {
			tdlist = ANTIC_GetDLWord(&tdlist);
			if (IR & 0x40) {
				printf("JVB %04X\n", tdlist);
				break;
			}
			printf("JMP %04X\n", tdlist);
		}
		else {
			if (IR & 0x40)
			printf("LMS %04X ", ANTIC_GetDLWord(&tdlist));
			if (IR & 0x20)
			printf("VSCROL ");
			if (IR & 0x10)
			printf("HSCROL ");
			printf("MODE %X\n", IR & 0x0f);
		}

		if (++nlines == 24) {
			if (pager())
			break;
			nlines = 0;
		}
	}
#else
	/* group identical instructions */
	UWORD tdlist = ANTIC_dlist;
	UWORD new_tdlist;
	UBYTE IR;
	int scrnaddr = -1;
	int nlines = 0;
	get_hex(&tdlist);
	new_tdlist = tdlist;
	IR = ANTIC_GetDLByte(&new_tdlist);
	for (;;) {
		printf("%04X: ", tdlist);
		if ((IR & 0x0f) == 0) {
			UBYTE new_IR;
			tdlist = new_tdlist;
			new_IR = ANTIC_GetDLByte(&new_tdlist);
			if (new_IR == IR) {
				int count = 1;
				do {
					count++;
					tdlist = new_tdlist;
					new_IR = ANTIC_GetDLByte(&new_tdlist);
				} while (new_IR == IR && count < 240);
				printf("%dx ", count);
			}
			if (IR & 0x80)
				printf("DLI ");
			printf("%c BLANK\n", ((IR >> 4) & 0x07) + '1');
			IR = new_IR;
		}
		else if ((IR & 0x0f) == 1) {
			tdlist = ANTIC_GetDLWord(&new_tdlist);
			if (IR & 0x80)
				printf("DLI ");
			if (IR & 0x40) {
				printf("JVB %04X\n", tdlist);
				break;
			}
			printf("JMP %04X\n", tdlist);
			new_tdlist = tdlist;
			IR = ANTIC_GetDLByte(&new_tdlist);
		}
		else {
			UBYTE new_IR;
			int new_scrnaddr;
			int count;
			if ((IR & 0x40) && scrnaddr < 0)
				scrnaddr = ANTIC_GetDLWord(&new_tdlist);
			for (count = 1;; count++) {
				tdlist = new_tdlist;
				new_IR = ANTIC_GetDLByte(&new_tdlist);
				if (new_IR != IR || count >= 240) {
					new_scrnaddr = -1;
					break;
				}
				if (IR & 0x40) {
					new_scrnaddr = ANTIC_GetDLWord(&new_tdlist);
					if (new_scrnaddr != scrnaddr)
						break;
				}
			}
			if (count > 1)
				printf("%dx ", count);
			if (IR & 0x80)
				printf("DLI ");
			if (IR & 0x40)
				printf("LMS %04X ", scrnaddr);
			if (IR & 0x20)
				printf("VSCROL ");
			if (IR & 0x10)
				printf("HSCROL ");
			printf("MODE %X\n", IR & 0x0f);
			scrnaddr = new_scrnaddr;
			IR = new_IR;
		}

		if (++nlines == 24) {
			if (pager())
				break;
			nlines = 0;
		}
	}
#endif
}

/* Sets the N flag to VAL, if it equals to 0 or 1. */
static void monitor_set_N(int const val)
{
	if (val == 0)
		CPU_ClrN;
	else if (val == 1)
		CPU_SetN;
}

/* Sets the V flag to VAL, if it equals to 0 or 1. */
static void monitor_set_V(int const val)
{
	if (val == 0)
		CPU_ClrV;
	else if (val == 1)
		CPU_SetV;
}

/* Sets the D flag to VAL, if it equals to 0 or 1. */
static void monitor_set_D(int const val)
{
	if (val == 0)
		CPU_ClrD;
	else if (val == 1)
		CPU_SetD;
}

/* Sets the I flag to VAL, if it equals to 0 or 1. */
static void monitor_set_I(int const val)
{
	if (val == 0)
		CPU_ClrI;
	else if (val == 1)
		CPU_SetI;
}

/* Sets the Z flag to VAL, if it equals to 0 or 1. */
static void monitor_set_Z(int const val)
{
	if (val == 0)
		CPU_ClrZ;
	else if (val == 1)
		CPU_SetZ;
}

/* Sets the C flag to VAL, if it equals to 0 or 1. */
static void monitor_set_C(int const val)
{
	if (val == 0)
		CPU_ClrC;
	else if (val == 1)
		CPU_SetC;
}

#ifdef MONITOR_TRACE
/* Opens/closes the 6502 trace file. */
static void set_trace_file(char const *filename)
{
	if (MONITOR_trace_file != NULL) {
		fclose(MONITOR_trace_file);
		printf("Trace file closed\n");
		MONITOR_trace_file = NULL;
	}
	if (filename != NULL) {
		MONITOR_trace_file = fopen(filename, "w");
		if (MONITOR_trace_file != NULL)
			printf("Trace file open\n");
		else
			perror(filename);
	}
}
#endif /* MONITOR_TRACE */

static void get_terminal_size(int *cols, int *rows) {
	*cols = 80;
	*rows = 24;

#ifdef TIOCGSIZE
	{
		struct ttysize ts;
		if(ioctl(STDIN_FILENO, TIOCGSIZE, &ts) == 0) {
			*cols = ts.ts_cols;
			*rows = ts.ts_lines;
		}
	}
#elif defined(TIOCGWINSZ)
	{
		struct winsize ts;
		if(ioctl(STDIN_FILENO, TIOCGWINSZ, &ts) == 0) {
			*cols = ts.ws_col;
			*rows = ts.ws_row;
		}
	}
#endif /* TIOCGSIZE */
}


#ifdef MONITOR_PROFILE
static void command_PROFILE(void)
{
	int i;
	for (i = 0; i < 24; i++) {
		int max, instr;
		int j;
		max = CPU_instruction_count[0];
		instr = 0;
		for (j = 1; j < 256; j++) {
			if (CPU_instruction_count[j] > max) {
				max = CPU_instruction_count[j];
				instr = j;
			}
		}
		if (max <= 0)
		break;
		CPU_instruction_count[instr] = 0;
		printf("Opcode %02X: %-9s has been executed %d times\n",
				instr, instr6502[instr], max);
	}
}

static void print_coverage_detail(UWORD addr)
{
	int len = 4;
#ifdef MONITOR_HINTS
	const char *label = find_label_name(addr, 0);
	if(label) {
		printf("%s ", label);
		len = strlen(label);
		if(len > 11) len = 11;
	} else {
		printf("%04X ", addr);
	}
#else
	printf("%04X ", addr);
#endif
	printf("%*lu(%5.2f%%) %8lu(%5.2f%%)",
			12 - len,
			MONITOR_coverage[addr].count,
			100.0f * (float)MONITOR_coverage[addr].count / (float)MONITOR_coverage_insns,
			MONITOR_coverage[addr].cycles,
			100.0f * (float)MONITOR_coverage[addr].cycles / (float)MONITOR_coverage_cycles);
	printf("  ");
	show_instruction(stdout, addr);
}

typedef struct {
	UWORD addr;
	unsigned long cycles;
} hog_rec;

#define HOG_MAX 10

/* Stupid simple insertion sort */
static void insert_hog_rec(hog_rec *list, unsigned int *count, UWORD addr, unsigned long cycles)
{
	int i, pos = -1;

	/* find where new entry should go (or -1 if it shouldn't) */
	for(i = 0; i < *count; i++) {
		if(cycles >= list[i].cycles) {
			pos = i;
			break;
		}
	}

	/* either the list isn't full (add this entry to the end), or else
		it's full and this entry sorts less than the stuff in the list. */
	if(pos == -1) {
		if(*count < HOG_MAX) {
			list[*count].addr = addr;
			list[*count].cycles = cycles;
			(*count)++;
		}
		return;
	}

	/* move list entries to make room for the new one */
	for(i = (*count) - 1; i >= pos; i--) {
		list[i + 1] = list[i];
	}
	list[pos].addr = addr;
	list[pos].cycles = cycles;
	if(*count < HOG_MAX) (*count)++;
}

#ifdef MONITOR_HINTS
/* If there's a label for addr, set 'label' to it.
	If not, look for a nearby one, and set 'label' to a string like
	"otherlabel+3" or "otherlabel-1". If there's nothing close by,
	just set 'label' to an empty string.
	Then append the hex address, so we get something like:
	"label/0600" or "label-1/05ff" or just "0600". */
static void get_nearby_label(UWORD addr, char *result) {
	int i = 0;
	const char *label = NULL;

	/* a label at addr is ideal. if there's not one, a label before
		addr is better than one after it, since it makes more sense to
		specify an unlabelled address as "subroutine+offset" or
		"loop+offset" than a negative offset from the next subroutine
		or loop. */
	for(i = 0; i > -129; i--) {
		label = find_label_name(addr + i, 0);
		if(label != NULL) break;
	}

	/* if nothing's in range, go ahead & look for a negative offset from
		a label at a later address. */
	if(label == NULL) {
		for(i = 1; i < 128; i++) {
			label = find_label_name(addr + i, 0);
			if(label != NULL) break;
		}
	}

	if(i == 0)
		snprintf(result, 127, "%s/%04X", label, addr); /* exact match */
	else if(label == NULL)
		snprintf(result, 127, "%04X", addr); /* got nothing... */
	else
		snprintf(result, 127, "%s%c%x/%04X", /* + or - offset */
				label,
				i < 0 ? '+' : '-',
				i < 0 ? -i : i,
				addr);
}
#endif

static void print_cov_range(UWORD start, UWORD end, int executed)
{
#ifdef MONITOR_HINTS
	char startlabel[128], endlabel[128];

	get_nearby_label(start, startlabel);
	get_nearby_label(end, endlabel);
	printf("%s => %s %sexecuted\n",
			startlabel, endlabel, executed ? "" : "NOT ");
#else
	printf("%04X => %04X %sexecuted\n",
			start, end, executed ? "" : "NOT ");
#endif
}

/* Handle the COV R command. Coalesces adjacent executed
	or non-executed instructions into ranges. "Instruction"
	here means opcode plus any operand: obviously if location N
	contains a 2- or 3-byte instruction, the operand bytes
	don't normally get executed by themselves, but we want to
	count them as "executed" if the opcode byte is. */
static void coverage_ranges(UWORD start, UWORD end)
{
	int range_start = start, range_end = start;
	int range_executed = MONITOR_coverage[start].count ? 1 : 0;

	while(range_start <= end) {
		int opsize = MONITOR_optype6502[MEMORY_SafeGetByte(range_end)] & 3;
		int executed = MONITOR_coverage[range_end].count ? 1 : 0;

		if((executed != range_executed) || (range_end > end)) {
			print_cov_range(range_start, range_end - 1, range_executed);
			range_start = range_end;
			range_executed = executed;
		}

		range_end += opsize;
	}
}

/* Show code coverage & profiling statistics.
	XXX: this code knows nothing about bankswitching! */
static void coverage(void)
{
	/* save these across calls, to save typing (and remembering) */
	static UWORD start = 0, end = 0xffff;
	static char cc = '?';

	unsigned int i, cov = 0, full = FALSE, hogs = FALSE, hog_count = 0, ranges = FALSE;
	unsigned long subttl_i = 0, subttl_c = 0;
	int detail_count = 0;
	hog_rec hog_list[HOG_MAX];
	char *cmd = get_token();

	if(cmd) cc = tolower(*cmd);

	if(cc == '?') {
		printf(
			"Usage:\n"
			"COV ?             - This help\n"
			"COV S [start end] - Summary of coverage stats\n"
			"COV H [start end] - Hogs: summary + top 10 CPU-using instructions\n"
			"COV F [start end] - Summary + detail for every instruction (long!)\n"
			"COV R [start end] - Show ranges of executed/non-executed code\n"
			"COV C             - Clear coverage\n");
		printf("With no argument, COV repeats the last S/H/F/R command.\n");
		printf("Without [start end], the S/H/F/R commands use the previous\n"
			"start/end addresses (or 0000 FFFF if not previously set)\n");
		printf("Current range: %04X %04X\n", start, end);
		return;
	} else if(cc == 'c') {
		MONITOR_coverage_insns = MONITOR_coverage_cycles = 0;
		memset(MONITOR_coverage, 0, sizeof(MONITOR_coverage));
		printf("Coverage stats reset\n");
		return;
	} else if(cc == 'f') {
		full = TRUE;
	} else if(cc == 's') {
		full = FALSE;
	} else if(cc == 'h') {
		full = FALSE;
		hogs = TRUE;
	} else if(cc == 'r') {
		ranges = TRUE;
	} else {
		printf("Invalid command, type \"COV ?\" for help\n");
		return;
	}

	if(!MONITOR_coverage_insns) {
		printf("No instructions executed since last reset\n");
		return;
	}

	get_hex(&start);
	get_hex(&end);

	if(ranges) {
		coverage_ranges(start, end);
		return;
	}

	if(full || hogs)
		printf("Addr  TimesExecuted    Cycles           Instruction\n");

	for(i = start; i <= end; i++) {
		subttl_i += MONITOR_coverage[i].count;
		subttl_c += MONITOR_coverage[i].cycles;

		if(MONITOR_coverage[i].count) {
			cov += MONITOR_optype6502[MEMORY_SafeGetByte(i)] & 3;

			if(full) {
				int cols, rows;
				get_terminal_size(&cols, &rows);
				if(detail_count++ >= rows - 1) {
					if(pager()) return;
					detail_count = 0;
				}
				print_coverage_detail(i);
			} else if(hogs) {
				insert_hog_rec(hog_list, &hog_count, i, MONITOR_coverage[i].cycles);
			}
		}
	}

	if(hogs) {
		for(i = 0; i < hog_count; i++) {
			print_coverage_detail(hog_list[i].addr);
		}
	}

	printf("Range %04x-%04x: %lu(%.2f%%) insns, %lu(%.2f%%) cycles executed\n",
			start,
			end,
			subttl_i,
			100.0f * (float)subttl_i / (float)MONITOR_coverage_insns,
			subttl_c,
			100.0f * (float)subttl_c / (float)MONITOR_coverage_cycles);
	printf("Coverage: %d(%.2f%%) of %d\n",
			cov,
			100.0f * (float)cov / (float)(end - start + 1),
			(end - start + 1));

	printf("Total: %lu instructions, %lu cycles executed\n",
			MONITOR_coverage_insns, MONITOR_coverage_cycles);
}
#endif /* MONITOR_PROFILE */

/* Displays current contents of the processor stack. */
static void show_stack(void)
{
	int ts;
	for (ts = 0x101 + CPU_regS; ts < 0x200; ) {
		if (ts < 0x1ff) {
			UWORD ta = (UWORD) (MEMORY_dGetWord(ts) - 2);
			if (MEMORY_dGetByte(ta) == 0x20) {
				printf("%04X: %02X %02X  %04X: JSR %04X\n",
					ts, MEMORY_dGetByte(ts), MEMORY_dGetByte(ts + 1), ta,
					MEMORY_dGetWord(ta + 1));
				ts += 2;
				continue;
			}
		}
		printf("%04X: %02X\n", ts, MEMORY_dGetByte(ts));
		ts++;
	}
}

/* Sets memory as read-only based on range fetched from command line. */
static void monitor_set_ROM(void)
{
	UWORD addr1;
	UWORD addr2;
	if (get_attrib_range(&addr1, &addr2)) {
		MEMORY_SetROM(addr1, addr2);
		printf("Changed memory from %04X to %04X into ROM\n",
			   addr1, addr2);
	}
}

/* Sets memory as writable based on range fetched from command line. */
static void monitor_set_RAM(void)
{
	UWORD addr1;
	UWORD addr2;
	if (get_attrib_range(&addr1, &addr2)) {
		MEMORY_SetRAM(addr1, addr2);
		printf("Changed memory from %04X to %04X into RAM\n",
			   addr1, addr2);
	}
}

#ifndef PAGED_ATTRIB
/* Sets memory as hardware based on range fetched from command line. */
static void monitor_set_hardware(void)
{
	UWORD addr1;
	UWORD addr2;
	if (get_attrib_range(&addr1, &addr2)) {
		MEMORY_SetHARDWARE(addr1, addr2);
		printf("Changed memory from %04X to %04X into HARDWARE\n",
			   addr1, addr2);
	}
}
#endif /* PAGED_ATTRIB */

#ifndef PAGED_MEM
/* Reads file into memory, under address fetched from command line. */
static void monitor_read_from_file(UWORD *addr)
{
	const char *filename;
	int xex = FALSE;
	/* Peek at the next token without retrieving it... */
	if(Util_strnicmp(token_ptr, "XEX ", 4) == 0) {
		xex = TRUE;
		/* we got the magic keyword, toss the token */
		(void)get_token();
	}

	filename = get_token();

	if (filename != NULL) {
		UWORD nbytes;
		if (xex) /* load xex file; no init nor run performed */
		{
			FIL f;
			FRESULT fr = f_open(&f, filename, FA_READ);
			if (fr != FR_OK) {
				perror(filename);
				return;
			}
			else {
				while (42) {
					UWORD fromaddr;
					UWORD toaddr;
					int byte;

					do {
						byte = _fgetc(&f);
						if (byte==EOF) { break; }
						fromaddr=byte&0xff;

						byte = _fgetc(&f);
						if (byte==EOF) { printf("Bad xex file\n"); break; }
						fromaddr|=((byte&0xff)<<8);

					} while (fromaddr==0xffff && byte !=EOF);

					if (byte==EOF) break;

					byte = _fgetc(&f);
					if (byte==EOF) { printf("Bad xex file\n"); break; }
					toaddr=byte&0xff;

					byte = _fgetc(&f);
					if (byte==EOF) { printf("Bad xex file\n"); break; }
					toaddr|=((byte&0xff)<<8);

					*addr=fromaddr; /* sets to last load addr */
					if ((int)toaddr-(int)fromaddr<0) { printf("Bad xex file\n"); break; }
					nbytes=toaddr-fromaddr+1;
					UINT rb;
					/* if not full block, error */
					if (f_read(&f, &MEMORY_mem[*addr], nbytes, &rb) != FR_OK) {
						printf("Bad xex file\n");
						break;
					}
					printf("Read dos block: %04X-%04X, %04X bytes. \n",fromaddr,toaddr, nbytes);
				}
				f_close(&f);
			}
			return;
		}
		else { /* !xex */
			if (get_hex(addr))
			{
				if (!get_hex(&nbytes))
					nbytes=0x10000-*addr;

				if (*addr + nbytes <= 0x10000) {
					FIL f;
					FRESULT fr = f_open(&f, filename, FA_READ);
					if (fr != FR_OK) {
						perror(filename);
						return;
					}
					else {
						UINT rb;
						/* read as many bytes as given or available */
						if (f_read(&f, &MEMORY_mem[*addr], nbytes, &rb) != FR_OK)
							printf("Could not read bytes\n");
						f_close(&f);
					}
					printf("Read %d bytes at %04X-%04X\n",nbytes,*addr,*addr+nbytes-1);
					return;
				}
			}
		}
	}
	printf("Bad arguments\n");
}

/* Writes memory to file, from address fetched from command line.
	Optionally, it can be a xex file, with optional run address.
 */
static void monitor_write_to_file(void)
{
	UWORD addr1;
	UWORD addr2;
	UWORD runaddr;
	int xex = FALSE, have_runaddr = FALSE;
#ifdef HAVE_POPEN
	int pipe = FALSE;
#endif

	/* Peek at the next token without retrieving it... */
	if(Util_strnicmp(token_ptr, "XEX ", 4) == 0) {
		xex = TRUE;
		/* we got the magic keyword, toss the token */
		(void)get_token();
	}

	if (get_hex2(&addr1, &addr2) && addr1 <= addr2) {
		size_t wbytes = 0;
		const char *filename;
		FIL f;
		filename = get_token();

		/* XXX this logic doesn't allow us to give a filename that
			matches a symbol name or hex address. */
		if(xex && filename != NULL && parse_hex(filename, &runaddr)) {
			have_runaddr = TRUE;
			filename = get_token();
		}

		if (filename == NULL) {
			filename = "memdump.dat";
#ifdef HAVE_POPEN
		} else if (filename[0] == '|') {
			if(filename[1]) {
				filename++;
			} else {
				filename = get_token();
			}
			if (filename == NULL) {
				printf("Missing argument after |\n");
				return;
			}
			pipe = TRUE;
#endif
		}

#ifdef HAVE_POPEN
		if(pipe)
			f = popen(filename, "w");
		else
#endif
		FRESULT fr = f_open(&f, filename, FA_WRITE | FA_CREATE_ALWAYS);
		if (fr != FR_OK) {
			perror(filename);
			return;
		} else {
			size_t nbytes = addr2 - addr1 + 1;

			if(xex) {
				fputc(0xff, &f); /* binary load FFFF header */
				fputc(0xff, &f);
				fputc(addr1 & 0xff, &f);
				fputc(addr1 >> 8, &f);
				fputc(addr2 & 0xff, &f);
				fputc(addr2 >> 8, &f);
				wbytes += 6;
			}
			UINT wb;
			if (f_write(&f, &MEMORY_mem[addr1], addr2 - addr1 + 1, &wb) != FR_OK)
				perror(filename);

			wbytes += nbytes;

			if(xex && have_runaddr) {
				fputc(0xe0, &f); /* start addr $02e0 = RUNAD */
				fputc(0x02, &f);
				fputc(0xe1, &f); /* end addr $02e1 */
				fputc(0x02, &f);
				fputc(runaddr & 0xff, &f);
				fputc(runaddr >> 8, &f);
				wbytes += 6;
			}
		}

#ifdef HAVE_POPEN
		if(pipe)
			pclose(f);
		else
#endif
			f_close(&f);

		/* TODO: when migrating to C99, instead of %lu and cast use %zu. */
		printf("Wrote %04X bytes to %s file '%s'",
				(unsigned int)wbytes, xex ? "XEX" : "RAW", filename);
		if(xex) {
			if(!have_runaddr) {
				printf(" (no run address)");
			} else {
				printf(", run address %04x", runaddr);
			}
		}
		putchar('\n');
	} else {
		printf("Invalid address range\n");
	}
}

/* Fills memory area witha value. Memory range and the value are fetched from
   command line. */
static void monitor_fill_mem(void)
{
	UWORD addr1;
	UWORD addr2;
	static UBYTE tab[64];
	UWORD hexval;
	if (get_hex3(&addr1, &addr2, &hexval)) {
		int n = 0;
		/* use int to avoid endless loop with addr2==0xffff */
		int a;
		int c=0;
		do {
			tab[n++] = (UBYTE) hexval;
			if (hexval > 0xff && n < 64)
				tab[n++] = (UBYTE) (hexval >> 8);
		} while (n < 64 && get_hex(&hexval));
		for (a = addr1; a <= addr2; a++) {
			MEMORY_dPutByte(a, tab[c++]);
			if (c>=n) c=0;
		}
		printf("Filled %04X-%04X with [",addr1,addr2);
		for (c=0; c<n; c++) printf("%s%02x",c?" ":"",tab[c]);
		printf("]\n");

		return;
	}
	printf("Bad arguments\n");
}

/* Changes memory contents. Start address and byte values are fetched from
   command line. */
static void monitor_change_mem(UWORD *addr)
{
	UWORD temp = 0;
	UWORD taddr=0;
	if (get_hex(addr)) {
		taddr=*addr;
		while (get_hex(&temp)) {
#ifdef PAGED_ATTRIB
			if (MEMORY_writemap[*addr >> 8] != NULL && MEMORY_writemap[*addr >> 8] != MEMORY_ROM_PutByte)
				(*MEMORY_writemap[*addr >> 8])(*addr, (UBYTE) temp);
#else
			if (MEMORY_attrib[*addr] == MEMORY_HARDWARE)
				MEMORY_HwPutByte(*addr, (UBYTE) temp);
#endif
			else /* RAM, ROM */
				MEMORY_dPutByte(*addr, (UBYTE) temp);
			(*addr)++;
			if (temp > 0xff) {
#ifdef PAGED_ATTRIB
				if (MEMORY_writemap[*addr >> 8] != NULL && MEMORY_writemap[*addr >> 8] != MEMORY_ROM_PutByte)
					(*MEMORY_writemap[*addr >> 8])(*addr, (UBYTE) (temp >> 8));
#else
				if (MEMORY_attrib[*addr] == MEMORY_HARDWARE)
					MEMORY_HwPutByte(*addr, (UBYTE) (temp >> 8));
#endif
				else /* RAM, ROM */
					MEMORY_dPutByte(*addr, (UBYTE) (temp >> 8));
				(*addr)++;
			}
		}
		printf("Changed %d bytes\n",*addr-taddr);
		return;
	}
	else
		printf("Bad arguments\n");
}
#endif /* PAGED_MEM */

/* Displays sum of a memory range, fetched from command line. */
static void monitor_sum_mem(void)
{
	UWORD addr1;
	UWORD addr2;
	if (get_hex2(&addr1, &addr2)) {
		int sum = 0;
		int i;
		for (i = addr1; i <= addr2; i++)
			sum += MEMORY_SafeGetByte(i);
		printf("SUM: %X\n", sum);
		return;
	}
	printf("Bad arguments\n");
}

/* Show memory contents, starting from address fetched from command line. */
static char screen_to_asc(char c);
static void monitor_show_mem(UWORD *addr, int screencodes)
{
	int count = 16;
	get_hex(addr);
	do {
		int i;
		printf("%04X: ", *addr);
		for (i = 0; i < 16; i++)
			printf("%02X ", MEMORY_SafeGetByte((UWORD) (*addr + i)));
		putchar(' ');
		for (i = 0; i < 16; i++) {
			UBYTE c;
			c = MEMORY_SafeGetByte(*addr);
			if(screencodes) c = screen_to_asc(c);
			(*addr)++;
			print_atascii_char(c);
		}
		putchar('\n');
	} while (--count > 0);
}

/* Starts searching for memory locations that hold a value fetched from command line. */
static void trainer_start_search(void)
{
	UWORD trainer_value = 0;
	int value_valid = get_hex(&trainer_value);

	/* alloc needed memory at first use */
	if (trainer_memory == NULL) {
		trainer_memory = (UBYTE *)Util_malloc(65536*2, "trainer_start_search"); // TODO: !!!
		if (trainer_memory != NULL) {
			trainer_flags = trainer_memory + 65536;
		} else {
			printf("Memory allocation failed!\n"
			"Trainer not available.\n");
		}
	}
	if (trainer_memory != NULL) {
		/* copy memory into shadow buffer at first use */
		long int count = 65535;
		do {
			*(trainer_memory+count) = MEMORY_SafeGetByte((UWORD) count);
			*(trainer_flags+count) = 0xff;
		} while (--count > -1);
		if (value_valid) {
			count = 65535;
			do {
				if (trainer_value != *(trainer_memory+count)) {
					*(trainer_flags+count) = 0;
				}
			} while (--count > -1);
		}
	}
}

/* Locates memory addresses that haven't changed since TSS. */
static void trainer_search_unchanged(void)
{
	UWORD trainer_value = 0;
	int value_valid = get_hex(&trainer_value);

	if (trainer_memory != NULL) {
		long int count = 65535;
		do {
			if (value_valid) {
				if (trainer_value != MEMORY_SafeGetByte((UWORD) count)) {
					*(trainer_flags+count) = 0;
				}
			} else {
				if (*(trainer_memory+count) != MEMORY_SafeGetByte((UWORD) count)) {
					*(trainer_flags+count) = 0;
				}
			}
			*(trainer_memory+count) = MEMORY_SafeGetByte((UWORD) count);
		} while (--count > -1);
	} else {
		printf("Use tss first.\n");
	}
}

/* Locates memory addresses that have changed since TSS. */
static void trainer_search_changed(void)
{
	UWORD trainer_value = 0;
	int value_valid = get_hex(&trainer_value);

	if (trainer_memory != NULL) {
		long int count = 65535;
		do {
			if (value_valid) {
				if (trainer_value != MEMORY_SafeGetByte((UWORD) count)) {
					*(trainer_flags+count) = 0;
				}
			} else {
				if (*(trainer_memory+count) == MEMORY_SafeGetByte((UWORD) count)) {
					*(trainer_flags+count) = 0;
				}
			};
			*(trainer_memory+count) = MEMORY_SafeGetByte((UWORD) count);
		} while (--count > -1);
	} else {
		printf("Use tss first.\n");
	}
}

/* Displays memory addresses located with TSS/TSN. */
static void trainer_print_addresses(void)
{
	UWORD addr_count_max = 0;
	int addr_valid = get_hex(&addr_count_max);

	/* default print size is 8*8 adresses */
	if (!addr_valid) {
		addr_count_max = 64;
	}

	if (trainer_memory != NULL) {
		long int count = 0;
		ULONG addr_count = 0;
		int i = 0;
		do {
			if (*(trainer_flags+count) != 0) {
				printf("%04X ", (UWORD) count);
				addr_count++;
				if (++i == 8) {
					printf("\n");
					i = 0;
				};
			};
		} while ((++count < 65536) && (addr_count < addr_count_max));
	printf("\n");
	} else {
		printf("Use tss first.\n");
	}
}

/* Searches in memory for a value. Memory range and value are fetched from
   command line. */
static void monitor_search_mem(void)
{
	/* static, so "S" without arguments repeats last search */
	static int n = 0;
	static UWORD addr1;
	static UWORD addr2;
	static UBYTE tab[64];
	UWORD hexval;
	if (get_hex3(&addr1, &addr2, &hexval)) {
		n = 0;
		do {
			tab[n++] = (UBYTE) hexval;
			if (hexval > 0xff && n < 64)
				tab[n++] = (UBYTE) (hexval >> 8);
		} while (n < 64 && get_hex(&hexval));
	} else {
		printf("Bad arguments\n");
		return;
	}
	if (n > 0) {
		int a;
		for (a = addr1; a <= addr2; a++) {
			int i = 0;
			while (MEMORY_SafeGetByte((UWORD) (a + i)) == tab[i]) {
				i++;
				if (i >= n) {
					printf("Found at %04X\n", a);
					break;
				}
			}
		}
	}
}

/* Shows disassembly of a loop that spans over address ADDR. */
static UWORD disassemble_loop(UWORD addr)
{
	int caddr;
	caddr = addr;
	for (;;) {
		UBYTE opcode;
		if (caddr > (UWORD) (addr + 0x7e)) {
			printf("Conditional loop containing instruction at %04X not detected\n", addr);
			break;
		}
		opcode = MEMORY_SafeGetByte(caddr);
		if ((opcode & 0x1f) == 0x10) {
			/* branch */
			UWORD target = caddr + 2 + (SBYTE) MEMORY_SafeGetByte(caddr + 1);
			if (target <= addr) {
				addr = disassemble(target);
				break;
			}
		}
		caddr += MONITOR_optype6502[opcode] & 3;
	}
	return addr;
}

#ifdef MONITOR_HINTS
static void configure_labels(UWORD *addr)
{
	char *cmd = get_token();
	if (cmd == NULL) {
		printf("Built-in labels are %sabled.\n", symtable_builtin_enable ? "en" : "dis");
		if (symtable_user_size > 0)
			printf("Using %d user-defined label%s.\n",
				symtable_user_size, (symtable_user_size > 1) ? "s" : "");
		else
			printf("There are no user-defined labels.\n");
		printf(
			"Labels are displayed in disassembly listings.\n"
			"You may also use them as command arguments"
#ifdef MONITOR_ASSEMBLER
				" and in the built-in assembler"
#endif
				".\n"
			"Usage:\n"
			"LABELS OFF            - no labels\n"
			"LABELS BUILTIN        - use only built-in labels\n"
			"LABELS LOAD filename  - use only labels loaded from file\n"
			"LABELS ADD filename   - use built-in and loaded labels\n"
			"LABELS SET name value - define a label\n"
			"LABELS LIST           - list user-defined labels\n"
		);
	}
	else {
		Util_strupper(cmd);
		if (strcmp(cmd, "OFF") == 0) {
			symtable_builtin_enable = FALSE;
			free_user_labels();
		}
		else if (strcmp(cmd, "BUILTIN") == 0) {
			symtable_builtin_enable = TRUE;
			free_user_labels();
		}
		else if (strcmp(cmd, "LOAD") == 0) {
			symtable_builtin_enable = FALSE;
			load_user_labels(get_token());
		}
		else if (strcmp(cmd, "ADD") == 0) {
			symtable_builtin_enable = TRUE;
			load_user_labels(get_token());
		}
		else if (strcmp(cmd, "SET") == 0) {
			const char *name = get_token();
			if (name != NULL && get_hex(addr)) {
				symtable_rec *p = find_user_label(name);
				if (p != NULL) {
					if (p->addr != *addr) {
						printf("%s redefined (previous value: %04X)\n", name, p->addr);
						p->addr = *addr;
					}
				}
				else
					add_user_label(name, *addr);
			}
			else
				printf("Missing or bad arguments\n");
		}
		else if (strcmp(cmd, "LIST") == 0) {
			int i;
			int nlines = 0;
			for (i = 0; i < symtable_user_size; i++) {
				if (++nlines == 24) {
					if (pager())
						break;
					nlines = 0;
				}
				printf("%04X %s\n", symtable_user[i].addr, symtable_user[i].name);
			}
		}
		else
			printf("Invalid command, type \"LABELS\" for help\n");
	}
}
#endif /* MONITOR_HINTS */

static void print_flags(char *buf, char* flags, UWORD value)
{
	int i;
	int l = strlen(flags);
	for (i = 0; i < l; i++)
		buf[i] = (value & (1 << (l-1-i)))
			? flags[i]
			: '_';
}

static void show_cartridge_info(CARTRIDGE_image_t *cart, char *desc)
{
	if (cart->type != CARTRIDGE_NONE) {
		printf("%s cartridge\n", desc);
		printf("Type:  %03i (%s)\n", cart->type, CARTRIDGES[cart->type].description);
		printf("Image: %s (%s)\n", cart->filename, cart->raw ? "RAW" : "CART");

		switch (cart->type) {
		case CARTRIDGE_RAMCART_64:
		case CARTRIDGE_RAMCART_128:
			printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
				(cart->state & 0x0002) ? "On " : "Off",
				((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
			printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
			printf("Register: %s\n", (cart->state & 0x0004) ? "Locked" : "Enabled");
			printf("Bank:     $%02X\n", (cart->state & 0x0038) >> 3);
			break;
		case CARTRIDGE_DOUBLE_RAMCART_256:
			printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
				(cart->state & 0x0002) ? "On " : "Off",
				((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
			printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
			if (cart->state & 0x2000) {
				printf( "Bank:     $%02X    128K Module Order: %s    256K mode\n",
					((cart->state & 0x0038) >> 3) | ((cart->state & 0x004) << 1),
					(cart->state & 0x4000) ? "Swapped" : "Normal " );
			}
			else {
				printf("Register: %s\n", (cart->state & 0x0004) ? "Locked" : "Enabled");
				printf( "Bank:     $%02X    128K Module: %i    2x128K mode\n",
					(cart->state & 0x0038) >> 3,
					(cart->state & 0x4000) >> 14 );
			}
			break;
		case CARTRIDGE_RAMCART_1M: {
				char switches[] = "---";
				printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
					(cart->state & 0x0002) ? "On " : "Off",
					((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
				printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
				if (cart->state & 0x8000) {
					print_flags(switches, "---", (cart->state | 0x00ff) >> 5);
					printf( "Bank:     $%02X    1M Module: $%02X (%s)   ABC jumpers installed\n",
						((cart->state & 0x0038) >> 3) | ((cart->state & 0x0004) << 1) | ((cart->state & 0x00c0) >> 2),
						((cart->state & 0x0300) >> 8),
						switches );
				}
				else {
					print_flags(switches, "CBA", ((cart->state & 0x0004) >> 2) | ((cart->state & 0x03c0) >> 5) );
					printf( "Bank:     $%02X    128K Module: $%02X (%s)\n",
						((cart->state & 0x0038) >> 3),
						((cart->state & 0x0004) >> 2) | ((cart->state & 0x03c0) >> 5),
						switches );
				}
			}
			break;
		case CARTRIDGE_RAMCART_2M: {
				char switches[] = "----";
				printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
					(cart->state & 0x0002) ? "On " : "Off",
					((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
				printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
				if (cart->state & 0x8000) {
					print_flags(switches, "1---", (cart->state | 0x00ff) >> 5);
					printf( "Bank:     $%02X    1M Module: $%02X (%s)   ABC jumpers installed\n",
						((cart->state & 0x0038) >> 3) | ((cart->state & 0x0004) << 1) | ((cart->state & 0x00c0) >> 2),
						((cart->state & 0x0300) >> 8),
						switches );
				}
				else {
					print_flags(switches, "1CBA", ((cart->state & 0x0004) >> 2) | ((cart->state & 0x03c0) >> 5) );
					printf( "Bank:     $%02X    128K Module: $%02X (%s)\n",
						((cart->state & 0x0038) >> 3),
						((cart->state & 0x0004) >> 2) | ((cart->state & 0x03c0) >> 5),
						switches );
				}
			}
			break;
		case CARTRIDGE_RAMCART_4M: {
				char switches[] = "-----";
				printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
					(cart->state & 0x0002) ? "On " : "Off",
					((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
				printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
				if (cart->state & 0x8000) {
					print_flags(switches, "2D---", (cart->state | 0x00ff) >> 5);
					printf( "Bank:     $%02X    1M Module: $%02X (%s)   ABC jumpers installed\n",
						((cart->state & 0x0038) >> 3) | ((cart->state & 0x0004) << 1) | ((cart->state & 0x00c0) >> 2),
						((cart->state & 0x0300) >> 8),
						switches );
				}
				else {
					print_flags(switches, "2DCBA", ((cart->state & 0x0004) >> 2) | ((cart->state & 0x03c0) >> 5) );
					printf( "Bank:     $%02X    128K Module: $%02X (%s)\n",
						((cart->state & 0x0038) >> 3),
						((cart->state & 0x0004) >> 2) | ((cart->state & 0x03c0) >> 5),
						switches );
				}
			}
			break;
		case CARTRIDGE_RAMCART_8M:
			printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
				(cart->state & 0x0002) ? "On " : "Off",
				((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
			printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
			printf( "Bank:     $%02X    1M Module: $%02X\n",
				((cart->state & 0x0038) >> 3) | ((cart->state & 0x0004) << 1) | ((cart->state & 0x00c0) >> 2),
				((cart->state & 0x0700) >> 8) );
			break;
		case CARTRIDGE_RAMCART_16M:
			printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
				(cart->state & 0x0002) ? "On " : "Off",
				((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
			printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
			printf( "Bank:     $%02X    1M Module: $%02X\n",
				((cart->state & 0x0038) >> 3) | ((cart->state & 0x0004) << 1) | ((cart->state & 0x00c0) >> 2),
				((cart->state & 0x0f00) >> 8) );
			break;
		case CARTRIDGE_RAMCART_32M:
			printf("Memory:   $8000-$9FFF: %s  $A000-$BFFF: %s\n",
				(cart->state & 0x0002) ? "On " : "Off",
				((cart->state & 0x0001) ^ ((cart->state & 0x1000) >> 12)) ? "Off" : "On " );
			printf("Access:   %s\n", (cart->state & 0x1000) ? "Read/Write" : "Read Only");
			printf( "Bank:     $%02X    1M Module: $%02X\n",
				((cart->state & 0x00038) >> 3) | ((cart->state & 0x00004) << 1) | ((cart->state & 0x000c0) >> 2),
				((cart->state & 0x00f00) >> 8) | ((cart->state & 0x40000) >> 14) );
			break;
		case CARTRIDGE_SIDICAR_32:
			printf("Memory:   $8000-$9FFF: %s\n", (cart->state & 0x10) ? "On" : "Off");
			printf("Bank:     $%02X\n", (cart->state & 0x03));
			break;
		default:
			printf("State: $%08X\n", cart->state);
		}
	}
}

/* Displays cartridge info. */
static void show_CARTRIDGE(void)
{
	show_cartridge_info(&CARTRIDGE_main, "Main");
	show_cartridge_info(&CARTRIDGE_piggyback, "Piggyback");
}

/* Displays current ANTIC state. */
static void show_ANTIC(void)
{
	printf("DMACTL=%02X    CHACTL=%02X    DLISTL=%02X    "
		   "DLISTH=%02X    HSCROL=%02X    VSCROL=%02X\n",
		   ANTIC_DMACTL, ANTIC_CHACTL, ANTIC_dlist & 0xff, ANTIC_dlist >> 8, ANTIC_HSCROL, ANTIC_VSCROL);
	printf("PMBASE=%02X    CHBASE=%02X    VCOUNT=%02X    "
		   "NMIEN= %02X    ypos=%4d\n",
		   ANTIC_PMBASE, ANTIC_CHBASE, ANTIC_GetByte(ANTIC_OFFSET_VCOUNT, TRUE), ANTIC_NMIEN, ANTIC_ypos);
}

/* Displays current PIA state. */
static void show_PIA(void)
{
	printf("PACTL= %02X    PBCTL= %02X    PORTA= %02X    "
		   "PORTB= %02X\n", PIA_PACTL, PIA_PBCTL, PIA_PORTA, PIA_PORTB);
}

/* Displays current GTIA state. */
static void show_GTIA(void)
{
	printf("HPOSP0=%02X    HPOSP1=%02X    HPOSP2=%02X    HPOSP3=%02X\n",
		   GTIA_HPOSP0, GTIA_HPOSP1, GTIA_HPOSP2, GTIA_HPOSP3);
	printf("HPOSM0=%02X    HPOSM1=%02X    HPOSM2=%02X    HPOSM3=%02X\n",
		   GTIA_HPOSM0, GTIA_HPOSM1, GTIA_HPOSM2, GTIA_HPOSM3);
	printf("SIZEP0=%02X    SIZEP1=%02X    SIZEP2=%02X    SIZEP3=%02X    SIZEM= %02X\n",
		   GTIA_SIZEP0, GTIA_SIZEP1, GTIA_SIZEP2, GTIA_SIZEP3, GTIA_SIZEM);
	printf("GRAFP0=%02X    GRAFP1=%02X    GRAFP2=%02X    GRAFP3=%02X    GRAFM= %02X\n",
		   GTIA_GRAFP0, GTIA_GRAFP1, GTIA_GRAFP2, GTIA_GRAFP3, GTIA_GRAFM);
	printf("COLPM0=%02X    COLPM1=%02X    COLPM2=%02X    COLPM3=%02X\n",
		   GTIA_COLPM0, GTIA_COLPM1, GTIA_COLPM2, GTIA_COLPM3);
	printf("COLPF0=%02X    COLPF1=%02X    COLPF2=%02X    COLPF3=%02X    COLBK= %02X\n",
		   GTIA_COLPF0, GTIA_COLPF1, GTIA_COLPF2, GTIA_COLPF3, GTIA_COLBK);
	printf("PRIOR= %02X    VDELAY=%02X    GRACTL=%02X\n",
		   GTIA_PRIOR, GTIA_VDELAY, GTIA_GRACTL);
}

/* Displays current POKEY state. */
static void show_POKEY(void)
{
	printf("AUDF1= %02X    AUDF2= %02X    AUDF3= %02X    AUDF4= %02X    AUDCTL=%02X    KBCODE=%02X\n",
		   POKEY_AUDF[POKEY_CHAN1], POKEY_AUDF[POKEY_CHAN2], POKEY_AUDF[POKEY_CHAN3], POKEY_AUDF[POKEY_CHAN4], POKEY_AUDCTL[0], POKEY_KBCODE);
	printf("AUDC1= %02X    AUDC2= %02X    AUDC3= %02X    AUDC4= %02X    IRQEN= %02X    IRQST= %02X\n",
		   POKEY_AUDC[POKEY_CHAN1], POKEY_AUDC[POKEY_CHAN2], POKEY_AUDC[POKEY_CHAN3], POKEY_AUDC[POKEY_CHAN4], POKEY_IRQEN, POKEY_IRQST);
	printf("SKSTAT=%02X    SKCTL= %02X\n", POKEY_SKSTAT, POKEY_SKCTL);
#ifdef STEREO_SOUND
	if (POKEYSND_stereo_enabled) {
		printf("Second chip:\n");
		printf("AUDF1= %02X    AUDF2= %02X    AUDF3= %02X    AUDF4= %02X    AUDCTL=%02X\n",
			   POKEY_AUDF[POKEY_CHAN1 + POKEY_CHIP2], POKEY_AUDF[POKEY_CHAN2 + POKEY_CHIP2], POKEY_AUDF[POKEY_CHAN3 + POKEY_CHIP2], POKEY_AUDF[POKEY_CHAN4 + POKEY_CHIP2], POKEY_AUDCTL[1]);
		printf("AUDC1= %02X    AUDC2= %02X    AUDC3= %02X    AUDC4= %02X\n",
			   POKEY_AUDC[POKEY_CHAN1 + POKEY_CHIP2], POKEY_AUDC[POKEY_CHAN2 + POKEY_CHIP2], POKEY_AUDC[POKEY_CHAN3 + POKEY_CHIP2], POKEY_AUDC[POKEY_CHAN4 + POKEY_CHIP2]);
	}
#endif
}

#ifndef BASIC
static void save_load_state(int save) {
	int result;
	char *filename;

	if( (filename = get_token()) == NULL ) filename = "monitor.a8s";
	if(save) {
		result = StateSav_SaveAtariState(filename, "wb", TRUE);
	} else {
		result = StateSav_ReadAtariState(filename, "rb");
		PLATFORM_Exit(FALSE);
	}

	printf("%s: %s\n", filename, result ? "OK" : "Failed");
}
#endif /* BASIC */

static char screen_to_asc(char c) {
	char bit7 = c & 0x80;
	c &= 0x7f;

	if(c < 64)
		c += 32;
	else if(c < 96)
		c -= 64;

	return c | bit7;
}

static char asc_to_screen(char c) {
	char bit7 = c & 0x80;
	c &= 0x7f;

	if(c < 32)
		c += 64;
	else if(c < 96)
		c -= 32;

	return c | bit7;
}

static void hex_to_asc(int screencodes) {
	UWORD c;
	while(get_hex(&c)) {
		if(screencodes) c = screen_to_asc(c);
		print_atascii_char(c);
	}
	putchar('\n');
}

static void asc_to_hex(int screencodes) {
	char *p = token_ptr;
	while(*p != '\0') {
		printf("%02x ", screencodes ? asc_to_screen(*p) : *p);
		p++;
	}
	putchar('\n');
}

static void hex_to_dec(UWORD val) {
	printf("$%04x = %d\n", val, val);
}

static void dec_to_hex(UWORD val) {
	printf("%d = $%04x\n", val, val);
}

static void bin_to_hex(void) {
	char *binval = get_token(), *p;
	UWORD result = 0;

	if(binval == NULL) {
		printf("Missing binary argument\n");
		return;
	}

	for(p = binval; *p != '\0'; p++) {
		if(*p != '1' && *p != '0') {
			printf("Invalid binary argument\n");
			return;
		}
		result <<= 1;
		if(*p == '1') result |= 1;
	}

	printf("%%%s = $%04x\n", binval, result);
}

static void hex_to_bin(UWORD val) {
	int mask;

	printf("$%04x = %%", val);

	mask = val < 0x100 ? 0x80 : 0x8000;
	while(mask) {
		putchar((val & mask) ? '1' : '0');
		mask >>= 1;
	}

	putchar('\n');
}

/* Prompt for search string. Returns TRUE if a string is entered,
	FALSE if the user just presses Return. */
static void prompt_for_string(char *buf) {
	char input[256];

	if(*buf) {
		printf("Search string [%s]: ", buf);
		safe_gets(input, 255, "");
	} else {
		safe_gets(input, 255, "Search string: ");
	}

	Util_strlcpy(buf, input, strlen(input) + 1);
}

/* Searches memory for a string, which can be given in ATASCII or
	screencodes. The arguments are optional, and saved between calls.
	To avoid ambiguity, the search string is prompted for separately.
	XXX because I use safe_gets(), the string can't have leading spaces.
 */
static void string_search(int screencodes)
{
	static char strbuf[256] = { 0 }; /* default = nothing (always prompt) */
	static UWORD start = 0, end = 0xffff; /* default = search all 64K */
	char bytes[256];
	int i, j, len;

	get_hex(&start);
	get_hex(&end);

	prompt_for_string(strbuf);
	len = strlen(strbuf);

	if(screencodes) {
		char *p, *q;
		for(p = strbuf, q = bytes; *p; p++, q++)
			*q = asc_to_screen(*p);
	} else {
		Util_strlcpy(bytes, strbuf, len + 1);
	}

	if(!bytes[0]) {
		printf("Missing search string\n");
		return;
	}

	printf("search %04x - %04x for '%s'\n", start, end, strbuf);
	for(i = start; i <= end; i++) {
		j = 0;
		while(MEMORY_SafeGetByte(i + j) == bytes[j]) {
			j++;
			if(j >= len) {
				printf("Found at %04X\n", i);
				break;
			}
		}
	}
}


/* Displays monitor help. */
static void show_help(void)
{
	printf(
		"CONT [addr]                    - Continue emulation (default addr=PC)\n"
		"SHOW                           - Show registers\n"
		"STACK                          - Show stack\n"
		"SET{PC,A,X,Y,S} hexval         - Set register value\n"
		"SET{N,V,D,I,Z,C} 0 or 1        - Set flag value\n"
		"C startaddr hexval...          - Change memory\n"
		"D [startaddr]                  - Disassemble memory\n"
		"F startaddr endaddr hexval...  - Fill memory\n");
	/* split into several printfs to avoid gcc -pedantic warning: "string length 'xxx'
	   is greater than the length '509' ISO C89 compilers are required to support" */
	printf(
		"M [startaddr]                  - Memory list\n"
		"MS [startaddr]                 - Memory list (show characters as screencodes)\n"
		"S startaddr endaddr hexval...  - Search memory\n"
		"SSTR startaddr endaddr string  - Search memory for ASCII string\n"
		"SSCR startaddr endaddr string  - Search memory for ANTIC screen-code string\n");
	printf(
		"LOOP [inneraddr]               - Disassemble a loop that contains inneraddr\n"
		"RAM startaddr endaddr          - Convert memory block into RAM\n"
		"ROM startaddr endaddr          - Convert memory block into ROM\n"
		"HARDWARE startaddr endaddr     - Convert memory block into HARDWARE\n"
		"CART                           - Show cartridge information\n");
	printf(
		"READ [XEX] filename or filename startaddr [nbytes]\n"
		"                               - Read file into memory\n"
		"                                 With XEX read Atari executable,\n"
		"                                 does not init nor run (useful for patches)\n");
	printf(
		"WRITE [XEX] startaddr endaddr [runaddr] [file]\n"
		"                               - Write memory block to a file (memdump.dat).\n"
		"                                 With XEX, writes an Atari executable with\n"
		"                                 optional run address (no init addr, sorry).\n"
#ifdef HAVE_POPEN
		"                                 [file] may begin with |, to pipe to a command\n"
#endif
		"SUM startaddr endaddr          - Print sum of specified memory range\n");
	if(pager()) return;
#ifdef MONITOR_TRACE
	printf(
		"TRACE [filename]               - Output 6502 trace on/off\n");
#endif
#ifdef MONITOR_BREAK
	printf(
		"BPC [addr]                     - Set breakpoint at address\n"
		"BLINE [ypos] or [1000+ypos]    - Break at scanline or blink scanline\n"
		"BBRK ON or OFF                 - Breakpoint on BRK on/off\n"
		"HISTORY or H                   - List last %d executed instructions\n", CPU_REMEMBER_PC_STEPS);
	printf(
		"JUMPS                          - List last %d executed JMP/JSR\n", CPU_REMEMBER_JMP_STEPS);
	printf(
		"G [addr]                       - Execute one instruction (default addr=PC)\n"
		"O [addr]                       - Step over the instruction (default addr=PC)\n"
		"R [addr]                       - Execute until return (default addr=PC)\n");
#elif !defined(NO_YPOS_BREAK_FLICKER)
	printf(
		"BLINE [1000+ypos]              - Blink scanline (8<=ypos<=247)\n");
#endif
	printf(
#ifdef MONITOR_BREAKPOINTS
		"B [argument...]                - Manage breakpoints (\"B ?\" for help)\n"
#endif
#ifdef MONITOR_ASSEMBLER
		"A [startaddr]                  - Start simple assembler\n"
#endif
		"ANTIC, GTIA, PIA, POKEY        - Display hardware registers\n"
		"DLIST [startaddr]              - Show Display List\n");
	printf(
#ifdef MONITOR_PROFILE
		"PROFILE                        - Display profiling statistics\n"
		"COV [argument...]              - Coverage statistics (\"COV ?\" for help)\n");
	printf(
#endif
#ifdef MONITOR_HINTS
		"LABELS [command] [filename]    - Configure labels\n"
#endif
		"TSS [value]                    - Start trainer search\n"
		"TSC [value]                    - Perform when trainer value has changed\n"
		"TSN [value]                    - Perform when trainer value has NOT changed\n"
		"                                 Without [value], perform a deep trainer search\n"
		"TSP [count]                    - Print [count] possible trainer addresses\n");
	printf(
		"COLDSTART, WARMSTART           - Perform system coldstart/warmstart\n"
#ifdef HAVE_SYSTEM
		"!command                       - Execute shell command\n"
#endif
		"DEC value                      - Convert hex value to decimal\n");
	if(pager()) return;
	printf(
		"HEX value                      - Convert decimal value to hex\n"
		"BIN value                      - Convert hex value to binary\n");
	printf(
		"BHEX value                     - Convert binary value to hex\n"
		"ASC value [value] ...          - Convert hex value(s) to ASCII string\n"
		"AHEX string                    - Convert ASCII string to hex\n"
		"SCR value [value] ...          - Convert hex value(s) to screencode string\n"
		"SHEX string                    - Convert screencode string to hex\n");
	printf(
      "FP number                      - Convert number to BCD floating point, show in hex\n"
      "FP exp m1 [m2 m3 m4 m5]        - Convert hex bytes to BCD float, show result\n"
		"                                 Missing mantissa bytes are assumed to be 00\n"
      "CFP addr number                - Convert to BCD float, store in memory\n"
		"MFP [addr]                     - Show memory as Atari BCD floating point\n"
		"                                 Default addr is d4 (FR0)\n");
	printf(
		"GRM addr [width] [height]      - Display memory as mono bitmap\n"
		"GRC addr [width] [height]      - Display memory as 4-color bitmap\n"
		"SAVESTATE [filename]           - Save machine state (default 'monitor.a8s')\n"
		"LOADSTATE [filename]           - Load machine state (default 'monitor.a8s')\n"
		"QUIT or EXIT                   - Quit emulator\n"
		"HELP or ?                      - This text\n");
}

static void print_gr_color(UWORD addr) {
	UBYTE b = MEMORY_SafeGetByte(addr);
	printf("%s", gr_color_chars[b >> 6]);
	printf("%s", gr_color_chars[(b >> 4) & 3]);
	printf("%s", gr_color_chars[(b >> 2) & 3]);
	printf("%s", gr_color_chars[b & 3]);
	printf("%s", gr_color_done);
}

static void print_gr_mono(UWORD addr) {
	UBYTE b = MEMORY_SafeGetByte(addr);
	int i;

	for(i = 0x80; i; i >>= 1)
		printf("%s", gr_color_chars[(b & i) ? 3 : 0]);

	printf("%s", gr_color_done);
}


static void print_graphics(int want_color) {
	static UWORD width = 1, height = 8;
	UWORD addr;
	int rows, cols, x, y, row = 0, col = 0;
	int trows, tcols;

	if(!get_hex(&addr)) {
		printf("Usage: GR%c addr [width-in-bytes] [height]\n", want_color ? 'C' : 'M' );
		return;
	}

	get_hex(&width);
	get_hex(&height);

	get_terminal_size(&tcols, &trows);
	cols = tcols / (width * (want_color ? 5 : 9));
	rows = trows / (height + 1);
	if(cols == 0) cols = 1;
	if(rows == 0) rows = 1;

	while(1) {
		for(row = 0; row < rows; row++) {
			for(col = 0; col < cols; col++) {
				printf("%04x", addr + col * width * height);
				for(x = 4; x < width * (want_color ? 4 : 8) + 1; x++)
					putchar('-');
			}
			putchar('\n');
			for(y = 0; y < height; y++) {
				for(col = 0; col < cols; col++) {
					putchar('|');
					for(x = 0; x < width; x++) {
					if(want_color)
						print_gr_color(addr + x + (col * width) * height);
					else
						print_gr_mono(addr + x + (col * width) * height);
					}
				}
				putchar('\n');
				addr += width;
			}
			addr += (cols - 1) * height * width;
		}
		if(pager()) break;
	}
}

/* See De Re Atari chapter 8 for a description of the FP storage format:
	http://www.atariarchives.org/dere/chapt08.php#H8_8 */
static double fp_to_double(unsigned char *fp, int *invalid) {
	int exp = (int)*fp, sign, i;
	unsigned char *mant = fp + 1;
	double fval = 0.0l, mult = 1.0l;

	*invalid = FALSE;

	/* "The number zero is handled as a special case, and is
		represented as a zero exponent and zero mantissa. Either the
		exponent or the first mantissa byte may be tested for zero." */
	if(exp == 0 || mant[0] == 0) return 0.0l;

	/* De Re doesn't mention it, but an exponent of $80 is invalid. It
		would mean "negative zero"... */
	if(exp == 0x80) {
		*invalid = TRUE;
		return 0.0l;
	}

	/* bit 7 of exponent is the sign of the mantissa, save & strip off */
	sign = (exp & 0x80 ? -1 : 1);
	exp &= 0x7f;

	/* The FP format looks like it should support exponents ranging $00-$7f,
		but the FP ROM limits the range to $0f-$70. Anything outside this, we
		flag as invalid (but the conversion result should be OK). */
	if(exp < 0x0f || exp > 0x70) *invalid = TRUE;

	/* "In excess 64 notation, the value 64 is added to the exponent
		value before it is placed in the exponent byte." */
	exp -= 64;

	/* Mantissa bytes are base 100, packed BCD, $00 - $99.
		"There is an implied decimal point to the right of the first mantissa
		byte", but it's not a decimal point (base 10), it's a centimal point
		(base 100).

		The 5 mantissa bytes are interpreted as:
		aa.bbccddee, so aa is taken as-is, bb is multiplied by 0.01, cc by 0.0001,
		etc.
	 */
	for(i = 0; i < 5; mant++, i++) {
		char hi, lo;

		hi = *mant >> 4;
		lo = *mant & 0x0f;

		/* Flag if either nybble isn't valid BCD, but finish the conversion */
		if(hi > 9 || lo > 9) *invalid = TRUE;

		fval += (double)(hi * 10 + lo) * mult;
		mult /= 100.0l;
	}

	/* Apply exponent and sign. */
	fval *= pow(100.0l, exp);
	fval *= (double)sign;

	return fval;
}

/* Convert an Atari FP number and print it, with a warning
	if the conversion found invalid input. */
static void print_fp_dbl(unsigned char *fp)
{
	int invalid = FALSE;
	printf("%-10g", fp_to_double(fp, &invalid));
	if(invalid) printf(" (invalid FP!)");
	putchar('\n');
}

/* Interpret memory as a 6-byte Atari floating point
	constant, display in decimal/scientific notation. */
static void mem_to_fp(void)
{
	UWORD addr;

	if(!get_hex(&addr)) addr = 0xd4; /* FR0 */

	print_fp_dbl(&MEMORY_mem[addr]);
}

/* Read 2 to 6 hex bytes from command line, interpret
	as a 6-byte Atari floating point constant, display
	in decimal/scientific notation. */
static void hex_to_fp(void)
{
	UWORD input;
	unsigned char buf[6];
	int i;

	for(i = 0; i < 6; i++) {
		if(get_hex(&input))
			buf[i] = (unsigned char)input;
		else
			buf[i] = 0;
	}

	print_fp_dbl(buf);
}

/* Base 100 logarithm, purely to make the code below more readable. */
#define LOG100(x) (log10(x) / log10(100.0l))

/* Convert a double to a 6-byte Atari BCD floating point value.
	Return TRUE for success, FALSE if conversion failed, due to
	the number being out of the Atari's range. */
static int double_to_fp(double val, unsigned char *result)
{
	double exp, mant;
	int i, sign = 1;
	long mantl;

	/* Set initial result value to zero. */
	for(i = 0; i < 6; i++)
		result[i] = 0;

	/* Logarithms not valid for negative numbers, so separate the
		sign and work on the absolute value. */
	if(val < 0) {
		val = fabs(val);
		sign = -1;
	}

	/* Calculate exponent and mantissa. Mantissa will always be >= 0
		and < 100. */
	exp = floor(LOG100(val));
	mant = val / pow(100.0l, exp);

	/* Paranoia, to make sure the loop below isn't infinite. This
		should never happen, but... */
	if(mant <= 0.0l) return TRUE;

	/* Normalize: shift left 1 base-100 place until 100 > mantissa >= 1.
		Each left shift means multiplying the mantissa by 100, so we
		have to decrement the (base-100) exponent. */
	while(mant < 1.0l) {
		mant *= 100.0l;
		exp--;
	}

	/* It's easier to convert to BCD if we use a long instead of a
		double, since C doesn't let us use modulus on a double. You
		can think of mantl as a fixed-point decimal number. */
	mantl = (long)floor(mant * 100000000.0l);

	/* At this point, result[] is still all zeroes. If we got a zero
		mantissa, we got a valid zero result. */
	if(mantl == 0) return TRUE;

	/* Convert to excess 64 notation and check range. The FP ROM limits
		the exponent to the range $0f-$70, even though the format should
		allow $00-$7f. Not sure why this was done (does it mask a bug?),
		but we do likewise here. Exponent too small just means we return
		a valid zero result, but too large is an error. */
	exp += 64;
	if(exp < 0x0f) return TRUE;
	if(exp > 0x70) return FALSE;

	/* Set sign bit. Can't use | on a double. */
	if(sign == -1) exp += 0x80;

	/* At this point we have a valid exponent and mantissa, so store
		them in result[]. */
	result[0] = (unsigned char)exp;

	/* Extract each base-100 digit, convert to BCD, store in result[]. */
	for(i = 5; i > 0; i--) {
		unsigned char n = mantl % 100l;
		mantl /= 100l;
		result[i] = ((n / 10) << 4) | (n % 10);
	}
	return TRUE;
}

/* Read a double from the command line, convert to Atari FP, print.
	If store is TRUE, read an address first, and store the FP bytes
	at the address (changes 6 bytes). */
static void fp_to_hex(int store) {
	UWORD addr;
	double val;
	char *t;
	unsigned char fp[6];
	char *end;
	int i;

	if(store) {
		if(!get_hex(&addr)) {
			printf("Missing/invalid address\n");
			return;
		}
	}

	if( (t = get_token()) == NULL) {
		printf("Missing floating point argument\n");
		return;
	}

	val = strtod(t, &end);
	if(end == t) {
		printf("Invalid floating point argument\n");
		return;
	}

	if(!double_to_fp(val, fp)) {
		printf("Floating point argument out of range\n");
		return;
	}

	if(store) printf("%04x: ", addr);
	for(i = 0; i < 6; i++) {
		printf("%02x ", fp[i]);
	}
	putchar('\n');

	if(store) {
		for(i = 0; i < 6; i++) {
			MEMORY_PutByte(addr, fp[i]);
			addr++;
		}
	}
}

#ifdef MONITOR_READLINE
#ifdef MONITOR_HINTS
/* 20170929 bkw: This code is based on the GNU Readline example code,
	found here:
	http://cnswww.cns.cwru.edu/php/chet/readline/readline.html#SEC49 */

/* Callback for readline, generates the list of monitor commands, one
	at a time. Not going to list all commands here, only ones >= 4 characters
	long. */
static char *command_generator(const char *text, int state)
{
	static int index, len;
	const char *name;

	static const char *commands[] = {
		"CONT", "SHOW", "STACK", "LOOP", "HARDWARE", "READ", "WRITE",
#ifdef MONITOR_TRACE
		"TRACE",
#endif
#if defined(MONITOR_BREAK) || !defined(NO_YPOS_BREAK_FLICKER)
		"BLINE",
#endif
#ifdef MONITOR_BREAK
		"BBRK", "HISTORY", "JUMPS",
#endif
		"ANTIC", "GTIA", "PIA", "POKEY", "DLIST",
#ifdef MONITOR_PROFILE
		"PROFILE",
#endif
		"LABELS",
		"SAVESTATE", "LOADSTATE",
		"COLDSTART", "WARMSTART", "QUIT", "EXIT", "HELP",
		NULL };

	if(!state)
	{
		index = 0;
		len = strlen(text);
	}

	while((name = commands[index]) != NULL)
	{
		index++;
		if(Util_strnicmp(name, text, len) == 0)
			return Util_strdup(name);
	}

	return (char *)NULL;
}

/* Callback for readline, generates the list of valid subcommands
	for the LABELS command */
static char *subcmd_labels_generator(const char *text, int state)
{
	static int index, len;
	const char *name;

	static const char *commands[] = {
		"OFF", "BUILTIN", "LOAD", "ADD", "SET", "LIST",
		NULL };

	if(!state)
	{
		index = 0;
		len = strlen(text);
	}

	while((name = commands[index]) != NULL)
	{
		index++;
		if(Util_strnicmp(name, text, len) == 0)
			return Util_strdup(name);
	}

	return (char *)NULL;
}

/* Callback for readline, generates the list of user labels (if any)
	followed by the list of builtin labels (if enabled). If no user labels
	are loaded, and builtin labels are disabled (LABELS OFF), this
	just returns NULL for every call. */
static char *label_generator(const char *text, int state)
{
	static int user_index, builtin_index, len;
	static const symtable_rec *builtins;
	char *name;

	if(!state)
	{
		user_index = 0;
		builtin_index = 0;
		builtins = (Atari800_machine_type == Atari800_MACHINE_5200 ? symtable_builtin_5200 : symtable_builtin);
		len = strlen(text);
	}

	while(user_index < symtable_user_size)
	{
		name = symtable_user[user_index].name;
		user_index++;
		if(Util_strnicmp(name, text, len) == 0)
			return Util_strdup(name);
	}

	while(symtable_builtin_enable && (builtins[builtin_index].name != NULL))
	{
		name = builtins[builtin_index].name;
		builtin_index++;
		if(Util_strnicmp(name, text, len) == 0)
			return Util_strdup(name);
	}

	return (char *)NULL;
}

/* Count instances of one or more consecutive spaces in rl_line_buffer.
	Leading spaces are ignored. */
static int count_spaces(void) {
	char *p = rl_line_buffer;
	int wasspace = 1, result = 0;

	while(*p != '\0') {
		if(*p == ' ') {
			if(!wasspace) {
				result++;
			}
			wasspace = 1;
		} else {
			wasspace = 0;
		}
		p++;
	}
	return result;
}

/* Return TRUE if we should complete filenames. Logic is rudimentary
	and gets confused if the cursor isn't actually at the end of the
	command line, or if someone says e.g. "labels LoAd".
	No harm done though (completion will fail, but that's it). */
static int cmd_wants_filename(void) {
	int spaces = count_spaces();

#ifdef HAVE_SYSTEM
	/* XXX for now, just filename completion. Maybe worth revisiting,
		do command completion using $PATH, for the first argument. */
	if(spaces >= 1 && rl_line_buffer[0] == '!')
		return TRUE;
#endif

	if(spaces == 1 && Util_strnicmp(rl_line_buffer, "read ", 5) == 0)
		return TRUE;

	if(spaces == 3 && Util_strnicmp(rl_line_buffer, "write ", 6) == 0)
		return TRUE;

#ifdef MONITOR_TRACE
	if(spaces == 1 && Util_strnicmp(rl_line_buffer, "trace ", 6) == 0)
		return TRUE;
#endif

#ifdef HAVE_STRSTR
	/* XXX For now, platforms that lack strstr() just can't complete
		filenames with 'labels add' or 'labels load'.
		If it weren't a GNU extension, it'd be nice to use strcasestr() here */
	if(spaces == 2 && Util_strnicmp(rl_line_buffer, "labels ", 7) == 0) {
		if(strstr(rl_line_buffer, " add ")  ||
		   strstr(rl_line_buffer, " ADD ")  ||
		   strstr(rl_line_buffer, " load ") ||
		   strstr(rl_line_buffer, " LOAD ") )
		{
			return TRUE;
		}
	}
#endif

	if(spaces == 1 && Util_strnicmp(rl_line_buffer, "savestate ", 10) == 0)
		return TRUE;

	if(spaces == 1 && Util_strnicmp(rl_line_buffer, "loadstate ", 10) == 0)
		return TRUE;


	return FALSE;
}

/* Callback for readline. Returns a list of matches, which can consist
	of monitor commands, labels (built-in or user), and/or filenames. */
static char **monitor_completion(const char *text, int start, int end)
{
	char **matches;

	/* setting this prevents readline from falling back to its default
		action, if this function returns NULL (no matches). The default
		action is to complete filenames. */
	rl_attempted_completion_over = TRUE;

	if(start == 0) {
		/* start of line is always a command... */
		matches = rl_completion_matches(text, command_generator);
	} else if(cmd_wants_filename()) {
		/* if we're expecting a filename, complete filenames (default action) */
		rl_attempted_completion_over = FALSE;
		return NULL;
	} else if(Util_strnicmp(rl_line_buffer, "labels ", 7) == 0) {
		matches = rl_completion_matches(text, subcmd_labels_generator);
	} else {
		/* ...otherwise assume it's a label. */
		matches = rl_completion_matches(text, label_generator);
	}

	return matches;
}
#endif /* MONITOR_HINTS */

/* Characters that signal a break between words, for readline.
	Someday this may be fancier, if we start supporting e.g. quoted
	strings or C-style expressions like (2+2)*2. */
static char *break_chars = " @*";

/* Setting rl_readline_name allows us to have our own private section
	in the user's ~/.inputrc. */
static void init_readline(void)
{
	static int need_init = TRUE;

	rl_completer_word_break_characters = break_chars;

	if(need_init)
	{
		need_init = FALSE;
		rl_readline_name = "Atari800";
#ifdef MONITOR_HINTS
		rl_attempted_completion_function = monitor_completion;
#endif
	}
}
#endif /* MONITOR_READLINE */

#ifdef MONITOR_HINTS
/* called from atari.c, for -label-file CLI arg. */
void MONITOR_PreloadLabelFile(char *filename)
{
	load_user_labels(filename);
}
#endif

#ifdef MONITOR_BREAK
/* called from atari.c, for -bbrk CLI arg. */
void MONITOR_BBRK_on(void)
{
	MONITOR_break_brk = TRUE;
}

/* called from atari.c, for -bpc CLI arg. */
void MONITOR_BPC(char *arg)
{
	UWORD addr = 0xd000;
	parse_hex(arg, &addr); /* XXX error message on bad arg? */
	MONITOR_break_addr = addr;
}
#endif

int MONITOR_Run(void)
{
	UWORD addr;

#ifdef __PLUS
	if (!Misc_AllocMonitorConsole(&mon_output, &mon_input))
		return TRUE;
#endif

#ifdef MONITOR_READLINE
	init_readline();
#endif

	addr = CPU_regPC;

	CPU_GetStatus();

	if (CPU_cim_encountered) {
		printf("(CIM encountered)\n");
		CPU_cim_encountered = FALSE;
	}

#ifdef MONITOR_BREAK
	if (break_over) {
		/* "O" command was active */
		MONITOR_break_addr = 0xd000;
		break_over = FALSE;
	}
	else if (CPU_regPC == MONITOR_break_addr)
		printf("(breakpoint at %04X)\n", (unsigned int) MONITOR_break_addr);
	else if (ANTIC_ypos == ANTIC_break_ypos)
		printf("(breakpoint at scanline %d)\n", ANTIC_break_ypos);
	else if (MONITOR_break_ret && MONITOR_ret_nesting <= 0)
		printf("(returned)\n");
	MONITOR_break_step = FALSE;
	MONITOR_break_ret = FALSE;
#endif /* MONITOR_BREAK */

	show_state();

	for (;;) {
		char s[128];
		static char old_s[128];
		char *t;

		safe_gets(s, sizeof(s), "> ");
		if (s[0] != '\0')
			strcpy(old_s, s);
		else {
			/* if no command is given, restart the last one, but remove all
			 * arguments, so after a 'm 600' we will see 'm 700' ... */
			int i;
			strcpy(s, old_s);
			for (i = 0; i < (int) sizeof(s); i++)
				if (s[i] == ' ') {
					s[i] = '\0';
					break;
				}
		}
#ifdef HAVE_SYSTEM
		if (s[0] == '!') {
			if (system(s + 1) == -1) {
				printf("Error executing '%s'\n", s+1);
			}
			continue;
		}
#endif
		token_ptr = s;
		t = get_token();
		if (t == NULL)
			continue;

		/* uppercase the command */
		Util_strupper(t);

		if (strcmp(t, "CONT") == 0) {
			if(get_hex(&addr)) CPU_regPC = addr;
#ifdef MONITOR_PROFILE
			memset(CPU_instruction_count, 0, sizeof(CPU_instruction_count));
#endif /* MONITOR_PROFILE */
			PLUS_EXIT_MONITOR;
			return TRUE;
		}
#ifdef MONITOR_BREAK
		else if (strcmp(t, "BBRK") == 0)
			monitor_break_BRK();
		else if (strcmp(t, "BPC") == 0)
			monitor_break_PC();
		else if (strcmp(t, "HISTORY") == 0 || strcmp(t, "H") == 0)
			show_history();
		else if (strcmp(t, "JUMPS") == 0)
			show_last_jumps();
		else if (strcmp(t, "G") == 0) {
			if(get_hex(&addr)) CPU_regPC = addr;
			MONITOR_break_step = TRUE;
			PLUS_EXIT_MONITOR;
			return TRUE;
		}
		else if (strcmp(t, "R") == 0 ) {
			if(get_hex(&addr)) CPU_regPC = addr;
			MONITOR_break_ret = TRUE;
			MONITOR_ret_nesting = 1;
			PLUS_EXIT_MONITOR;
			return TRUE;
		}
		else if (strcmp(t, "O") == 0) {
			if(get_hex(&addr)) CPU_regPC = addr;
			get_hex(&addr);
			step_over();
			PLUS_EXIT_MONITOR;
			return TRUE;
		}
#endif /* MONITOR_BREAK */
#if defined(MONITOR_BREAK) || !defined(NO_YPOS_BREAK_FLICKER)
		else if (strcmp(t, "BLINE") == 0)
			monitor_bline();
#endif /* defined(MONITOR_BREAK) || !defined(NO_YPOS_BREAK_FLICKER) */
		else if (strcmp(t, "DLIST") == 0)
			show_dlist();
		else if (strcmp(t, "SETPC") == 0)
			get_uword(&CPU_regPC);
		else if (strcmp(t, "SETS") == 0)
			get_ubyte(&CPU_regS);
		else if (strcmp(t, "SETA") == 0)
			get_ubyte(&CPU_regA);
		else if (strcmp(t, "SETX") == 0)
			get_ubyte(&CPU_regX);
		else if (strcmp(t, "SETY") == 0)
			get_ubyte(&CPU_regY);
		else if (strcmp(t, "SETN") == 0)
			monitor_set_N(get_bool());
		else if (strcmp(t, "SETV") == 0)
			monitor_set_V(get_bool());
		else if (strcmp(t, "SETD") == 0)
			monitor_set_D(get_bool());
		else if (strcmp(t, "SETI") == 0)
			monitor_set_I(get_bool());
		else if (strcmp(t, "SETZ") == 0)
			monitor_set_Z(get_bool());
		else if (strcmp(t, "SETC") == 0)
			monitor_set_C(get_bool());
#ifdef MONITOR_TRACE
		else if (strcmp(t, "TRACE") == 0) {
			const char *filename = get_token();
			set_trace_file(filename);
		}
#endif /* MONITOR_TRACE */
#ifdef MONITOR_PROFILE
		else if (strcmp(t, "PROFILE") == 0)
			command_PROFILE();
		else if (strcmp(t, "COV") == 0)
			coverage();
#endif /* MONITOR_PROFILE */
		else if (strcmp(t, "SHOW") == 0)
			show_state();
		else if (strcmp(t, "STACK") == 0)
			show_stack();
		else if (strcmp(t, "ROM") == 0)
			monitor_set_ROM();
		else if (strcmp(t, "RAM") == 0)
			monitor_set_RAM();
#ifndef PAGED_ATTRIB
		else if (strcmp(t, "HARDWARE") == 0)
			monitor_set_hardware();
#endif /* PAGED_ATTRIB */
		else if (strcmp(t, "CART") == 0)
			show_CARTRIDGE();
		else if (strcmp(t, "COLDSTART") == 0) {
			Atari800_Coldstart();
			PLUS_EXIT_MONITOR;
			return TRUE;	/* perform reboot immediately */
		}
		else if (strcmp(t, "WARMSTART") == 0) {
			Atari800_Warmstart();
			PLUS_EXIT_MONITOR;
			return TRUE;	/* perform reboot immediately */
		}
#ifndef PAGED_MEM
		else if (strcmp(t, "READ") == 0)
			monitor_read_from_file(&addr);
		else if (strcmp(t, "WRITE") == 0)
			monitor_write_to_file();
		else if (strcmp(t, "F") == 0)
			monitor_fill_mem();
		else if (strcmp(t, "C") == 0)
			monitor_change_mem(&addr);
#endif /* PAGED_MEM */
		else if (strcmp(t, "SUM") == 0)
			monitor_sum_mem();
		else if (strcmp(t, "M") == 0)
			monitor_show_mem(&addr, FALSE);
		else if (strcmp(t, "MS") == 0)
			monitor_show_mem(&addr, TRUE);
		else if (strcmp(t, "TSS") == 0)
			trainer_start_search();
		else if (strcmp(t, "TSN") == 0)
			trainer_search_unchanged();
		else if (strcmp(t, "TSC") == 0)
			trainer_search_changed();
		else if (strcmp(t, "TSP") == 0)
			trainer_print_addresses();
		else if (strcmp(t, "S") == 0)
			monitor_search_mem();
#ifdef MONITOR_BREAKPOINTS
		else if (strcmp(t, "B") == 0)
			monitor_breakpoints();
#endif
		else if (strcmp(t, "D") == 0) {
			get_hex(&addr);
			addr = disassemble(addr);
		}
		else if (strcmp(t, "LOOP") == 0) {
			get_hex(&addr);
			addr = disassemble_loop(addr);
		}
#ifdef MONITOR_HINTS
		else if (strcmp(t, "LABELS") == 0)
			configure_labels(&addr);
#endif
		else if (strcmp(t, "ANTIC") == 0)
			show_ANTIC();
		else if (strcmp(t, "PIA") == 0)
			show_PIA();
		else if (strcmp(t, "GTIA") == 0)
			show_GTIA();
		else if (strcmp(t, "POKEY") == 0)
			show_POKEY();
#ifdef MONITOR_ASSEMBLER
		else if (strcmp(t, "A") == 0) {
			get_hex(&addr);
			addr = assembler(addr);
		}
#endif
		else if (strcmp(t, "DEC") == 0) {
			if(get_hex(&addr))
				hex_to_dec(addr);
			else
				printf("Missing/invalid hex argument\n");
		} else if (strcmp(t, "HEX") == 0) {
			int d;
			if(get_dec(&d))
				dec_to_hex(d);
			else
				printf("Missing/invalid decimal argument\n");
		} else if (strcmp(t, "BIN") == 0) {
			if(get_hex(&addr))
				hex_to_bin(addr);
			else
				printf("Missing/invalid hex argument\n");
		} else if (strcmp(t, "BHEX") == 0) {
			bin_to_hex();
		} else if (strcmp(t, "AHEX") == 0) {
			asc_to_hex(FALSE);
		} else if (strcmp(t, "ASC") == 0) {
			hex_to_asc(FALSE);
		} else if (strcmp(t, "SHEX") == 0) {
			asc_to_hex(TRUE);
		} else if (strcmp(t, "SCR") == 0) {
			hex_to_asc(TRUE);
		} else if (strcmp(t, "GRC") == 0) {
			print_graphics(TRUE);
		} else if (strcmp(t, "GRM") == 0) {
			print_graphics(FALSE);
#ifndef BASIC
		} else if (strcmp(t, "SAVESTATE") == 0) {
			save_load_state(TRUE);
		} else if (strcmp(t, "LOADSTATE") == 0) {
			save_load_state(FALSE);
#endif /* BASIC */
		} else if (strcmp(t, "SSTR") == 0) {
			string_search(FALSE);
		} else if (strcmp(t, "SSCR") == 0) {
			string_search(TRUE);
		} else if (strcmp(t, "MFP") == 0) {
			mem_to_fp();
		} else if (strcmp(t, "CFP") == 0) {
			fp_to_hex(TRUE);
		} else if (strcmp(t, "FP") == 0) {
			int spaces = 0;
			char *p = token_ptr;
			while(*p++) spaces += (*p == ' ');
			/* if we got at least 2 args, assume the input is hex bytes.
				for 1 arg, assume it's a floating point number. */
			if(spaces)
				hex_to_fp();
			else
				fp_to_hex(FALSE);
		} else if (strcmp(t, "HELP") == 0 || strcmp(t, "?") == 0)
			show_help();
		else if (strcmp(t, "QUIT") == 0 || strcmp(t, "EXIT") == 0) {
			PLUS_EXIT_MONITOR;
			return FALSE;
		} else if(t[0] == '*' || t[0] == '@') {
			UWORD val;
			if(parse_hex(t, &val)) {
				printf("%s = $%04x\n", t, val);
			} else {
				printf("Invalid dereference\n");
			}
		} else {
			printf("Invalid command!\n");
		}
	}
}

/*
vim:ts=4:sw=4:
*/
