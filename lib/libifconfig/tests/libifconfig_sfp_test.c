/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price
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

#include <atf-c.h>

#include <libifconfig.h>
#include <libifconfig_sfp.h>

/*
 * SFF-8472 module temperature is a signed 16-bit twos complement value in
 * units of 1/256 degree Celsius.  Every expected value below is an exact
 * multiple of 1/256, so it is exactly representable as a double.
 */

ATF_TC_WITHOUT_HEAD(temp_C_positive);
ATF_TC_BODY(temp_C_positive, tc)
{
	ATF_CHECK_EQ(0.0, temp_C(0x0000));
	ATF_CHECK_EQ(25.0, temp_C(0x1900));
	ATF_CHECK_EQ(25.5, temp_C(0x1980));
	ATF_CHECK_EQ(125.0, temp_C(0x7d00));
}

ATF_TC_WITHOUT_HEAD(temp_C_negative);
ATF_TC_BODY(temp_C_negative, tc)
{
	ATF_CHECK_EQ(-1.0, temp_C(0xff00));
	ATF_CHECK_EQ(-10.5, temp_C(0xf580));
	ATF_CHECK_EQ(-40.0, temp_C(0xd800));
}

ATF_TC_WITHOUT_HEAD(temp_C_extremes);
ATF_TC_BODY(temp_C_extremes, tc)
{
	ATF_CHECK_EQ(-128.0, temp_C(0x8000));
	ATF_CHECK_EQ(127.99609375, temp_C(0x7fff));
	ATF_CHECK_EQ(-1.0 / 256, temp_C(0xffff));
}

/*
 * The tables below are generated from lib/libifconfig/sfp.lua.  Codes are
 * spelled numerically so that a test binary can be built against an
 * unpatched table set.  An unlisted code looks up as NULL, so route every
 * description through a placeholder rather than dereferencing it.
 */
static const char *
unlisted(const char *description)
{
	return (description != NULL ? description : "(unlisted)");
}

ATF_TC_WITHOUT_HEAD(sff8024_identifiers);
ATF_TC_BODY(sff8024_identifiers, tc)
{
	/* SFF-8024 Table 4-1: 19h is OSFP, not QSFP. */
	ATF_CHECK_STREQ("OSFP8X", unlisted(ifconfig_sfp_id_display(0x19)));
	ATF_CHECK_STREQ("QSFP+(CMIS)", unlisted(ifconfig_sfp_id_display(0x1E)));

	/* Codes assigned after SFF-8024 rev 4.6. */
	ATF_CHECK_STREQ("SFP-DD(CMIS)", unlisted(ifconfig_sfp_id_display(0x1F)));
	ATF_CHECK_STREQ("SFP+(CMIS)", unlisted(ifconfig_sfp_id_display(0x20)));
	ATF_CHECK_STREQ("OSFP-XD", unlisted(ifconfig_sfp_id_display(0x21)));
	ATF_CHECK_STREQ("OIF-ELSFP", unlisted(ifconfig_sfp_id_display(0x22)));
	ATF_CHECK_STREQ("XPO", unlisted(ifconfig_sfp_id_display(0x26)));
}

ATF_TC_WITHOUT_HEAD(sff8024_cmis_identifiers);
ATF_TC_BODY(sff8024_cmis_identifiers, tc)
{
	ATF_CHECK(ifconfig_sfp_id_is_cmis(0x1F));
	ATF_CHECK(ifconfig_sfp_id_is_cmis(0x20));
	ATF_CHECK(ifconfig_sfp_id_is_cmis(0x21));
	ATF_CHECK(ifconfig_sfp_id_is_cmis(0x22));
	ATF_CHECK(ifconfig_sfp_id_is_cmis(0x1E));

	/* A plain SFP is still managed by SFF-8472. */
	ATF_CHECK(!ifconfig_sfp_id_is_cmis(0x03));
}

ATF_TC_WITHOUT_HEAD(sff8024_connectors);
ATF_TC_BODY(sff8024_connectors, tc)
{
	/* SFF-8024 rev 4.7 renamed 26h from Mini CS to SN. */
	ATF_CHECK_STREQ("SN (previously Mini CS) optical connector",
	    unlisted(ifconfig_sfp_conn_description(0x26)));
	ATF_CHECK_STREQ("MPO 1x16 Parallel Optic",
	    unlisted(ifconfig_sfp_conn_description(0x28)));
}

ATF_TC_WITHOUT_HEAD(sff8024_extended_compliance);
ATF_TC_BODY(sff8024_extended_compliance, tc)
{
	ATF_CHECK_STREQ("10 Mb/s Single Pair Ethernet",
	    unlisted(ifconfig_sfp_eth_ext_description(0x0E)));
	ATF_CHECK_STREQ("100GBASE-SR1, CAUI-4 (no FEC)",
	    unlisted(ifconfig_sfp_eth_ext_description(0x28)));
	ATF_CHECK_STREQ("100GBASE-CR1/200GBASE-CR2/400GBASE-CR4",
	    unlisted(ifconfig_sfp_eth_ext_description(0x3F)));
	ATF_CHECK_STREQ("400GBASE-DR4",
	    unlisted(ifconfig_sfp_eth_ext_description(0x47)));
	ATF_CHECK_STREQ("50GBASE-ER",
	    unlisted(ifconfig_sfp_eth_ext_description(0x4A)));
	ATF_CHECK_STREQ("256GFC-SW4 (FC-PI-7P)",
	    unlisted(ifconfig_sfp_eth_ext_description(0x7F)));
	ATF_CHECK_STREQ("64GFC (FC-PI-7)",
	    unlisted(ifconfig_sfp_eth_ext_description(0x80)));
	ATF_CHECK_STREQ("128GFC (FC-PI-8)",
	    unlisted(ifconfig_sfp_eth_ext_description(0x81)));
}

ATF_TC_WITHOUT_HEAD(sff8024_cmis_smf);
ATF_TC_BODY(sff8024_cmis_smf, tc)
{
	/* 25h was reclaimed as Reserved in SFF-8024 rev 4.8. */
	ATF_CHECK_STREQ("(unlisted)",
	    unlisted(ifconfig_sfp_cmis_smf_description(0x25)));

	ATF_CHECK_STREQ("128GFC-CWDM4",
	    unlisted(ifconfig_sfp_cmis_smf_description(0x26)));
	ATF_CHECK_STREQ("100GBASE-ZR",
	    unlisted(ifconfig_sfp_cmis_smf_description(0x44)));
	ATF_CHECK_STREQ("400GBASE-ZR",
	    unlisted(ifconfig_sfp_cmis_smf_description(0x4D)));
	ATF_CHECK_STREQ("800GBASE-DR8",
	    unlisted(ifconfig_sfp_cmis_smf_description(0x56)));
	ATF_CHECK_STREQ("1.6TBASE-DR8",
	    unlisted(ifconfig_sfp_cmis_smf_description(0x7F)));
}

ATF_TC_WITHOUT_HEAD(sff8024_cmis_mmf);
ATF_TC_BODY(sff8024_cmis_mmf, tc)
{
	/* Renamed to the Clause 167 spellings in SFF-8024 rev 4.10. */
	ATF_CHECK_STREQ("100GBASE-SR1",
	    unlisted(ifconfig_sfp_cmis_mmf_description(0x0D)));
	ATF_CHECK_STREQ("400GBASE-SR4",
	    unlisted(ifconfig_sfp_cmis_mmf_description(0x11)));

	ATF_CHECK_STREQ("200GBASE-SR2",
	    unlisted(ifconfig_sfp_cmis_mmf_description(0x1B)));
	ATF_CHECK_STREQ("100GBASE-VR1",
	    unlisted(ifconfig_sfp_cmis_mmf_description(0x1D)));
	ATF_CHECK_STREQ("1.6T-SR8.2",
	    unlisted(ifconfig_sfp_cmis_mmf_description(0x24)));
}

ATF_TC_WITHOUT_HEAD(sff8472_transceiver_codes);
ATF_TC_BODY(sff8472_transceiver_codes, tc)
{
	/* Table 5-3, byte 9. */
	ATF_CHECK_STREQ("Multimode 62.5um (M6)",
	    unlisted(ifconfig_sfp_fc_media_description(0x08)));
	ATF_CHECK_STREQ("Multimode 50um (M5, M5E)",
	    unlisted(ifconfig_sfp_fc_media_description(0x04)));

	/* Table 5-3, byte 10 bit 1 defers to byte 62. */
	ATF_CHECK_STREQ("See byte 62, Fibre Channel Speed 2",
	    unlisted(ifconfig_sfp_fc_speed_description(0x02)));

	/* Table 5-3, bytes 7-8: active is bit 3, passive is bit 2. */
	ATF_CHECK_STREQ("Active Cable",
	    unlisted(ifconfig_sfp_cab_tech_description(0x0008)));
	ATF_CHECK_STREQ("Passive Cable",
	    unlisted(ifconfig_sfp_cab_tech_description(0x0004)));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, temp_C_positive);
	ATF_TP_ADD_TC(tp, temp_C_negative);
	ATF_TP_ADD_TC(tp, temp_C_extremes);
	ATF_TP_ADD_TC(tp, sff8024_identifiers);
	ATF_TP_ADD_TC(tp, sff8024_cmis_identifiers);
	ATF_TP_ADD_TC(tp, sff8024_connectors);
	ATF_TP_ADD_TC(tp, sff8024_extended_compliance);
	ATF_TP_ADD_TC(tp, sff8024_cmis_smf);
	ATF_TP_ADD_TC(tp, sff8024_cmis_mmf);
	ATF_TP_ADD_TC(tp, sff8472_transceiver_codes);

	return (atf_no_error());
}
