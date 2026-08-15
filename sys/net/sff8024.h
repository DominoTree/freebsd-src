/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 George V. Neville-Neil
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef	_NET_SFF8024_H_
#define	_NET_SFF8024_H_

#include <sys/types.h>

/*
 * The following set of constants are from Document SFF-8024
 * "SFF Module Management Reference Code Tables" revision 4.14
 * published by SNIA on June 4, 2026.
 *
 * SFF-8024 is the reference source for the identifiers assigned to
 * interpret the memory maps of self-identifying modules.  A module
 * reports the same identifier, connector, encoding and extended
 * compliance codes whether it is read through SFF-8472, SFF-8436 or
 * CMIS, so the values belong to none of those maps.  Only the tables
 * FreeBSD decodes are transcribed here.
 */

/*
 * Table 4-1 Identifier values.  SFF-8472 table 5-1 and the SFF-8436
 * module type byte both defer to this table.
 */
enum {
	SFF_8024_ID_UNKNOWN	= 0x0, /* Unknown or unspecified */
	SFF_8024_ID_GBIC	= 0x1, /* GBIC */
	SFF_8024_ID_SFF		= 0x2, /* Module soldered to motherboard (ex: SFF)*/
	SFF_8024_ID_SFP		= 0x3, /* SFP or SFP “Plus” */
	SFF_8024_ID_XBI		= 0x4, /* 300 pin XBI */
	SFF_8024_ID_XENPAK	= 0x5, /* Xenpak */
	SFF_8024_ID_XFP		= 0x6, /* XFP */
	SFF_8024_ID_XFF		= 0x7, /* XFF */
	SFF_8024_ID_XFPE	= 0x8, /* XFP-E */
	SFF_8024_ID_XPAK	= 0x9, /* XPAk */
	SFF_8024_ID_X2		= 0xA, /* X2 */
	SFF_8024_ID_DWDM_SFP	= 0xB, /* DWDM-SFP */
	SFF_8024_ID_QSFP	= 0xC, /* QSFP */
	SFF_8024_ID_QSFPPLUS	= 0xD, /* QSFP+ or later */
	SFF_8024_ID_CXP		= 0xE, /* CXP */
	SFF_8024_ID_HD4X	= 0xF, /* Shielded Mini Multilane HD 4X */
	SFF_8024_ID_HD8X	= 0x10, /* Shielded Mini Multilane HD 8X */
	SFF_8024_ID_QSFP28	= 0x11, /* QSFP28 or later */
	SFF_8024_ID_CXP2	= 0x12, /* CXP2 (aka CXP28) */
	SFF_8024_ID_CDFP	= 0x13, /* CDFP (Style 1/Style 2) */
	SFF_8024_ID_SMM4	= 0x14, /* Shielded Mini Multilate HD 4X Fanout */
	SFF_8024_ID_SMM8	= 0x15, /* Shielded Mini Multilate HD 8X Fanout */
	SFF_8024_ID_CDFP3	= 0x16, /* CDFP (Style3) */
	SFF_8024_ID_MICROQSFP	= 0x17, /* microQSFP */
	SFF_8024_ID_QSFP_DD	= 0x18, /* QSFP-DD 8X Pluggable Transceiver */
	SFF_8024_ID_OSFP8X	= 0x19, /* OSFP 8X Pluggable Transceiver */
	SFF_8024_ID_SFP_DD	= 0x1A, /* SFP-DD 2X Pluggable Transceiver */
	SFF_8024_ID_DSFP	= 0x1B, /* DSFP Dual SFF Pluggable Transceiver */
	SFF_8024_ID_X4ML	= 0x1C, /* x4 MiniLink/OcuLink */
	SFF_8024_ID_X8ML	= 0x1D, /* x8 MiniLink */
	SFF_8024_ID_QSFP_CMIS	= 0x1E, /* QSFP+ or later w/ Common Management
					   Interface Specification */
	SFF_8024_ID_SFP_DD_CMIS	= 0x1F, /* SFP-DD 2X Pluggable Transceiver
					   w/ CMIS */
	SFF_8024_ID_SFPPLUS_CMIS = 0x20, /* SFP+ or later w/ CMIS */
	SFF_8024_ID_OSFP_XD	= 0x21, /* OSFP-XD w/ CMIS */
	SFF_8024_ID_ELSFP	= 0x22, /* OIF-ELSFP w/ CMIS */
	SFF_8024_ID_CDFP_X4_PCIE = 0x23, /* CDFP (x4 PCIe) SFF-TA-1032
					    w/ CMIS */
	SFF_8024_ID_CDFP_X8_PCIE = 0x24, /* CDFP (x8 PCIe) SFF-TA-1032
					    w/ CMIS */
	SFF_8024_ID_CDFP_X16_PCIE = 0x25, /* CDFP (x16 PCIe) SFF-TA-1032
					     w/ CMIS */
	SFF_8024_ID_XPO		= 0x26, /* XPO */
	SFF_8024_ID_LAST	= SFF_8024_ID_XPO
};

#if defined(_WANT_SFF_8024_ID) || defined(_WANT_SFF_8472_ID)
static const char *sff_8024_id[SFF_8024_ID_LAST + 1] = {
	"Unknown",
	"GBIC",
	"SFF",
	"SFP/SFP+/SFP28",
	"XBI",
	"Xenpak",
	"XFP",
	"XFF",
	"XFP-E",
	"XPAK",
	"X2",
	"DWDM-SFP/SFP+",
	"QSFP",
	"QSFP+",
	"CXP",
	"HD4X",
	"HD8X",
	"QSFP28",
	"CXP2",
	"CDFP",
	"SMM4",
	"SMM8",
	"CDFP3",
	"microQSFP",
	"QSFP-DD",
	"OSFP8X",
	"SFP-DD",
	"DSFP",
	"x4MiniLink/OcuLink",
	"x8MiniLink",
	"QSFP+(CMIS)",
	"SFP-DD(CMIS)",
	"SFP+(CMIS)",
	"OSFP-XD",
	"OIF-ELSFP",
	"CDFP(x4 PCIe)",
	"CDFP(x8 PCIe)",
	"CDFP(x16 PCIe)",
	"XPO"
};

static inline const char *
sff_8024_id_name(uint8_t id)
{

	if (id <= SFF_8024_ID_LAST)
		return (sff_8024_id[id]);
	return (id >= 0x80 ? "Vendor specific" : "Reserved");
}
#endif

/*
 * Table 4-4 Extended Specification Compliance Codes.  The byte holding
 * one is an enumerated value, not a bit field.  Only the two BASE-T
 * codes are transcribed; they are the only place a BASE-T module
 * identifies itself, as SFF-8472 table 5-3 has no BASE-T code.
 */
#define SFF_8024_EXT_10G_T_SFI	0x16 /* 10GBASE-T, SFI electrical interface */
#define SFF_8024_EXT_10G_T_SR	0x1c /* 10GBASE-T Short Reach (30 m) */

#endif /* !_NET_SFF8024_H_ */
