#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zpool_scrub/zpool_scrub.cfg

#
# DESCRIPTION:
#	Verify error scrub reports an empty error log instead of exiting
#	silently with success.
#
# STRATEGY:
#	1. Create a pool with nothing in its error log.
#	2. Request an error scrub.
#	3. Verify it fails and says why.
#

verify_runnable "global"

function cleanup
{
	destroy_pool $TESTPOOL2
	rm -f $TESTDIR/vdev_a
}

log_onexit cleanup

log_assert "Error scrub reports an empty error log."

truncate -s $MINVDEVSIZE $TESTDIR/vdev_a
log_must zpool create -f $TESTPOOL2 $TESTDIR/vdev_a

log_mustnot zpool scrub -e $TESTPOOL2
log_must eval "zpool scrub -e $TESTPOOL2 2>&1 | \
    grep 'no recorded errors to scrub'"

log_pass "Error scrub reports an empty error log."
