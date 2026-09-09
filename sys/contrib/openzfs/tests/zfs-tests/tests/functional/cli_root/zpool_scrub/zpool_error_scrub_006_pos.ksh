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
#	Verify 'zpool status -v' explains an error log whose entries no longer
#	resolve to a file, rather than printing an empty file list.
#
# STRATEGY:
#	1. Create a pool and a file in it.
#	2. Zinject errors and read the file so they are logged.
#	3. Remove the file, leaving the injection active so the recorded
#	   blocks can be neither resolved nor healed.
#	4. Verify status -v says so and does not promise a file list.
#

verify_runnable "global"

function cleanup
{
	zinject -c all
	destroy_pool $TESTPOOL2
	rm -f $TESTDIR/vdev_a $TESTDIR/status.out
}

log_onexit cleanup

log_assert "status -v explains an error log that resolves to no files."

truncate -s $MINVDEVSIZE $TESTDIR/vdev_a
log_must zpool create -f -O primarycache=none $TESTPOOL2 $TESTDIR/vdev_a
typeset file=/$TESTPOOL2/$TESTFILE0
log_must dd if=/dev/urandom of=$file bs=1M count=16

log_must zinject -t data -e checksum -f 100 -a $file
dd if=$file of=/dev/null bs=1M count=16
log_must sync_pool $TESTPOOL2

# Remove the file while the injection is still active, so the recorded blocks
# resolve to no name and cannot be healed.  Reading the log heals what it can,
# so capture it once and assert against that.
log_must rm -f $file
log_must sync_pool $TESTPOOL2

log_must eval "zpool status -v $TESTPOOL2 > $TESTDIR/status.out"
log_must grep -q "no longer" $TESTDIR/status.out
log_mustnot grep -q "following files" $TESTDIR/status.out

log_pass "status -v explains an error log that resolves to no files."
