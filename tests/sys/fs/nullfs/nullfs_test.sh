#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Nick Price
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
# These tests focus on nullfs single-file mounts, where the mount root is a
# VREG vnode with VV_ROOT set and namei leaves ni_dvp == vp_crossmp.  Kernel
# paths that assume a mount point is a directory misbehave for such mounts.

# mount_nullfs a file over a file and record it for cleanup.
filemount_mount() {
	atf_check mount_nullfs "$1" "$2"
	echo "$2" >>mounts
}

# Undo whatever a test set up, tolerating partial failures.
filemount_cleanup() {
	if [ -f fast_lookup.saved ]; then
		sysctl vfs.cache.param.fast_lookup="$(cat fast_lookup.saved)" \
		    >/dev/null 2>&1 || true
	fi
	if [ -f sleeper.pid ]; then
		kill "$(cat sleeper.pid)" 2>/dev/null || true
	fi
	if [ -f mounts ]; then
		tail -r mounts | while read -r m; do
			umount "$m" 2>/dev/null || umount -f "$m" 2>/dev/null || true
		done
	fi
}

atf_test_case filemount_basic cleanup
filemount_basic_head() {
	atf_set "descr" "A file mount exposes the source file's contents"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_basic_body() {
	printf 'source\n' >src
	printf 'target\n' >target
	filemount_mount src target
	atf_check -o inline:'source\n' cat target
	atf_check umount target
	# Original contents reappear once unmounted.
	atf_check -o inline:'target\n' cat target
}
filemount_basic_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_emptydir cleanup
filemount_emptydir_head() {
	atf_set "descr" "mount -o emptydir on a file mount fails, does not panic"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_emptydir_body() {
	printf a >src
	printf b >target
	echo target >>mounts
	# emptydir asks the kernel to readdir a VREG; it must be rejected,
	# not tripped through vn_dir_check_empty()'s VDIR assertion.
	atf_check -s not-exit:0 -e ignore mount_nullfs -o emptydir src target
}
filemount_emptydir_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_fullpath_slowpath cleanup
filemount_fullpath_slowpath_head() {
	atf_set "descr" "Slow-path vn_fullpath reports the mount, not the source"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_fullpath_slowpath_body() {
	printf a >src
	printf b >target
	filemount_mount src target
	# Force the non-SMR fallback so vn_fullpath_any() is exercised.
	sysctl -n vfs.cache.param.fast_lookup >fast_lookup.saved
	atf_check -o ignore sysctl vfs.cache.param.fast_lookup=0
	exec 9<target
	procstat -f $$ >pstat.out
	exec 9<&-
	got=$(awk '$3 == 9 { print $NF }' pstat.out)
	# The fd must resolve to the mount path, not the source file.
	atf_check_equal "$(realpath .)/target" "$got"
}
filemount_fullpath_slowpath_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_exec_procpath cleanup
filemount_exec_procpath_head() {
	atf_set "descr" "kern.proc.pathname of a binary run from a file mount"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_exec_procpath_body() {
	atf_check cp -p /usr/bin/sleep src
	printf placeholder >victim
	filemount_mount src victim
	# Relative exec of a file mount stores vp_crossmp as p_textdvp; a
	# later kern.proc.pathname must resolve it rather than panic.
	./victim 30 &
	pid=$!
	echo "$pid" >sleeper.pid
	atf_check -o match:'/victim$' procstat -b "$pid"
	kill "$pid"
	wait "$pid" 2>/dev/null || true
	rm -f sleeper.pid
}
filemount_exec_procpath_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_realpath cleanup
filemount_realpath_head() {
	atf_set "descr" "realpath(1) of a file mount returns the mount path"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_realpath_body() {
	printf a >src
	printf b >target
	filemount_mount src target
	atf_check -o inline:"$(realpath .)/target\n" realpath target
}
filemount_realpath_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_stacking cleanup
filemount_stacking_head() {
	atf_set "descr" "Stacking a file mount on a file mount is rejected"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_stacking_body() {
	printf a >src
	printf b >mid
	printf c >top
	filemount_mount src mid
	atf_check -s not-exit:0 -e ignore mount_nullfs top mid
}
filemount_stacking_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_nlink cleanup
filemount_nlink_head() {
	atf_set "descr" "A file with nlink > 1 cannot be file-mounted"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_nlink_body() {
	printf a >src
	atf_check ln src src.link
	printf b >target
	atf_check -s not-exit:0 -e ignore mount_nullfs src target
}
filemount_nlink_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_self cleanup
filemount_self_head() {
	atf_set "descr" "A file cannot be file-mounted over itself"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_self_body() {
	printf a >src
	echo src >>mounts
	atf_check -s not-exit:0 -e ignore mount_nullfs src src
}
filemount_self_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_rename_target cleanup
filemount_rename_target_head() {
	atf_set "descr" "Renaming onto a file mount is rejected, does not panic"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_rename_target_body() {
	printf a >src
	printf b >target
	printf c >other
	filemount_mount src target
	# The target resolves to the mount root with tdvp == vp_crossmp.
	atf_check -s not-exit:0 -e ignore mv other target
}
filemount_rename_target_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_rename_source cleanup
filemount_rename_source_head() {
	atf_set "descr" "Renaming a file mount root is rejected, does not panic"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_rename_source_body() {
	printf a >src
	printf b >target
	filemount_mount src target
	# The source resolves to the mount root with fdvp == vp_crossmp.
	atf_check -s not-exit:0 -e ignore mv target renamed
}
filemount_rename_source_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_trailing_slash cleanup
filemount_trailing_slash_head() {
	atf_set "descr" "A trailing slash on a file mount yields ENOTDIR"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_trailing_slash_body() {
	printf a >src
	printf b >target
	filemount_mount src target
	atf_check -s not-exit:0 -e ignore cat target/
}
filemount_trailing_slash_cleanup() {
	filemount_cleanup
}

atf_test_case filemount_notdir cleanup
filemount_notdir_head() {
	atf_set "descr" "A file mount cannot be used as a directory"
	atf_set "require.user" "root"
	atf_set "require.kmods" "nullfs"
}
filemount_notdir_body() {
	printf a >src
	printf b >target
	filemount_mount src target
	# chdir into the mount and a lookup beneath it must both fail ENOTDIR.
	atf_check -s not-exit:0 -e ignore sh -c 'cd target'
	atf_check -s not-exit:0 -e ignore cat target/child
}
filemount_notdir_cleanup() {
	filemount_cleanup
}

atf_init_test_cases() {
	atf_add_test_case filemount_basic
	atf_add_test_case filemount_emptydir
	atf_add_test_case filemount_fullpath_slowpath
	atf_add_test_case filemount_exec_procpath
	atf_add_test_case filemount_realpath
	atf_add_test_case filemount_stacking
	atf_add_test_case filemount_nlink
	atf_add_test_case filemount_self
	atf_add_test_case filemount_rename_target
	atf_add_test_case filemount_rename_source
	atf_add_test_case filemount_trailing_slash
	atf_add_test_case filemount_notdir
}
