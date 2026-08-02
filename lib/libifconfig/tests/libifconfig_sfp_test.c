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

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, temp_C_positive);
	ATF_TP_ADD_TC(tp, temp_C_negative);
	ATF_TP_ADD_TC(tp, temp_C_extremes);

	return (atf_no_error());
}
