#!/bin/sh
#
# Copyright (c) 2016 Will Andrews
# Copyright (c) 2025 The FreeBSD Foundation
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer
#    in this position and unchanged.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
# IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
# NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
#
##
# Install a boot environment using the most recently built packages
# from the source tree's object directory (make packages).  Note that
# the script does not verify that the packages match the current
# source tree checkout -- it always uses the latest built packages.
#
# Requires: a fully built set of packages (make packages), pkg
#
# In a sandbox for the new boot environment, this script configures a
# temporary local package repository and installs the base system
# packages.  It also runs etcupdate and optionally upgrades third-party
# packages.  Upon successful completion, the system will be ready to
# boot into the new boot environment.  Upon failure, the target boot
# environment will be destroyed.  In all cases, the running system is
# left untouched.
#
## Usage:
# bepkginstall
#
## User modifiable variables - set these in the environment if desired.
# Utility to manage ZFS boot environments.
BE_UTILITY="${BE_UTILITY:-"bectl"}"
# If not empty, 'pkg upgrade' of third-party packages will be skipped.
NO_PKG_UPGRADE="${NO_PKG_UPGRADE:-""}"
# Config updater - 'etcupdate' is supported.  Set to an empty string to skip.
CONFIG_UPDATER="${CONFIG_UPDATER:-"etcupdate"}"
# Flags for etcupdate if used.
ETCUPDATE_FLAGS="${ETCUPDATE_FLAGS:-"-BF"}"


########################################################################
## Functions
cleanup() {
	[ -z "${cleanup_commands}" ] && return
	echo "Cleaning up ..."
	# shellcheck disable=SC2086 # intentional word splitting
	for command in ${cleanup_commands}; do
		# shellcheck disable=SC2086 # intentional word splitting
		${command}
	done
}

errx() {
	cleanup
	echo "error: $*" >&2
	exit 1
}

rmdir_be() {
	chflags -R noschg "${BE_MNTPT}"
	rm -rf "${BE_MNTPT}"
}

unmount_be() {
	mount | grep " on ${BE_MNTPT}" | awk '{print $3}' | sort -r | \
	    xargs -t umount -f
}

cleanup_be() {
	# Before destroying, unmount any child filesystems that may have
	# been mounted under the boot environment.  Sort them in reverse
	# order so children are unmounted first.
	unmount_be
	# Clean up any directories that were created by the operation.
	if [ -n "${created_be_dirs}" ]; then
		for dir in ${created_be_dirs}; do
			rm -rf "${BE_MNTPT}${dir}"
		done
	fi
	${BE_UTILITY} destroy -F "${BENAME}"
}

cleanup_repo_conf() {
	rm -f "${BE_MNTPT}/etc/pkg/FreeBSD-local-base.conf"
}

create_be_dirs() {
	echo "${BE_MNTPT}: Inspecting dirs $*"
	for dir in "$@"; do
		curdir="$dir"
		topdir="$dir"
		while :; do
			[ -e "${BE_MNTPT}${curdir}" ] && break
			topdir="$curdir"
			curdir=$(dirname "${curdir}")
		done
		[ "$curdir" = "$dir" ] && continue

		# Add the top-most nonexistent directory to the list, then
		# mkdir -p the innermost directory specified by the argument.
		# This way the least number of directories are rm'd directly.
		created_be_dirs="${topdir} ${created_be_dirs}"
		echo "${BE_MNTPT}: Created ${dir}"
		mkdir -p "${BE_MNTPT}${dir}" || return $?
	done
	return 0
}

update_etcupdate() {
	# Run etcupdate from the host with -D to target the BE, avoiding
	# chroot which would fail if the BE has newer binaries than the
	# running kernel.
	etcupdate_cmd="${srcdir}/usr.sbin/etcupdate/etcupdate.sh"
	# shellcheck disable=SC2086 # intentional word splitting of ETCUPDATE_FLAGS
	"${etcupdate_cmd}" -s "${srcdir}" -D "${BE_MNTPT}" ${ETCUPDATE_FLAGS} || return $?
	"${etcupdate_cmd}" resolve -D "${BE_MNTPT}"
}

# Special command-line subcommand that can be used to do a full cleanup
# after a manual post-mortem has been completed.
postmortem() {
	[ -n "${BENAME}" ] || errx "Must specify BENAME"
	[ -n "${BE_MNTPT}" ] || errx "Must specify BE_MNTPT"
	echo "Performing post-mortem on BE ${BENAME} at ${BE_MNTPT} ..."
	unmount_be
	rmdir_be
	echo "Post-mortem cleanup complete."
	echo "To destroy the BE (recommended), run: ${BE_UTILITY} destroy ${BENAME}"
	echo "To instead continue with the BE, run: ${BE_UTILITY} activate ${BENAME}"
}

if [ -n "$BEINSTALL_CMD" ]; then
	${BEINSTALL_CMD} "$@"
	exit $?
fi

########################################################################
## Main

if [ "$(basename -- "${BE_UTILITY}")" = "bectl" ]; then
	${BE_UTILITY} check || errx "${BE_UTILITY} sanity check failed"
fi

cleanup_commands=""
trap 'errx "Interrupt caught"' HUP INT TERM

[ "$(whoami)" != "root" ] && errx "Must be run as root"

[ ! -f "Makefile.inc1" ] && errx "Must be in FreeBSD source tree"
srcdir=$(pwd)
objtop=$(make -V OBJTOP 2>/dev/null)
[ ! -d "${objtop}" ] && errx "Must have built FreeBSD from source tree"

# Locate the package repository produced by 'make packages'.
objroot=$(make -V OBJROOT 2>/dev/null)
repodir="${objroot}repo"
[ ! -d "${repodir}" ] && errx "Package repo not found at ${repodir}; run 'make packages' first"

# ABI from staged world; fall back to host uname for native kernel-only builds.
pkg_abi_file="${objtop}/worldstage/usr/bin/uname"
[ -f "${pkg_abi_file}" ] || pkg_abi_file="/usr/bin/uname"
pkg_abi=$(pkg -o ABI_FILE="${pkg_abi_file}" config ABI 2>/dev/null)
[ -z "${pkg_abi}" ] && errx "Unable to determine ABI from pkg"

# The repo should have a 'latest' symlink under the ABI directory.
if [ -L "${repodir}/${pkg_abi}/latest" ]; then
	local_repo="${repodir}/${pkg_abi}/latest"
elif [ -d "${repodir}/${pkg_abi}" ]; then
	# Find the first (likely only) version directory.
	# shellcheck disable=SC2012 # ls is fine here; paths are controlled
	local_repo=$(ls -d "${repodir}/${pkg_abi}"/*/ 2>/dev/null | head -1)
	[ -z "${local_repo}" ] && errx "No package version directory found in ${repodir}/${pkg_abi}/"
	local_repo="${local_repo%/}"
else
	errx "No packages found for ABI ${pkg_abi} in ${repodir}"
fi

# Verify the repo has a catalogue.
[ ! -f "${local_repo}/packagesite.pkg" ] && \
	errx "Repository catalogue not found in ${local_repo}; was 'make packages' completed?"

echo "Using package repository: ${local_repo}"

# Verify this system uses pkgbase.
pkg info -e FreeBSD-runtime 2>/dev/null || \
	errx "This system does not appear to use pkgbase (FreeBSD-runtime not installed)"

# Determine which kernel to install from the build configuration.
# The first KERNCONF entry is used as the install kernel.
kern_installkernel=$(make -V KERNCONF 2>/dev/null | awk '{print $1}')
[ -z "${kern_installkernel}" ] && errx "Unable to determine KERNCONF"
kern_pkg_name=$(echo "${kern_installkernel}" | tr '[:upper:]' '[:lower:]')
echo "Kernel to install: ${kern_installkernel} (FreeBSD-kernel-${kern_pkg_name})"

# Determine BE name from the built freebsd-version.
commit_ver=$("${objtop}/bin/freebsd-version/freebsd-version" -u 2>/dev/null)
[ -z "${commit_ver}" ] && errx "Unable to determine FreeBSD version"

# Use the git commit timestamp for the BE name.
commit_time=$(git show -s --format='%ct' 2>/dev/null) || \
	errx "Unable to determine git commit timestamp"
commit_ts=$(date -r "${commit_time}" '+%Y%m%d.%H%M%S')

BENAME="${commit_ver}-${commit_ts}"

# If a BE with this name already exists, append an incrementing suffix.
if ${BE_UTILITY} list -H 2>/dev/null | awk '{print $1}' | grep -qx "${BENAME}"; then
	be_idx=2
	while ${BE_UTILITY} list -H 2>/dev/null | awk '{print $1}' | grep -qx "${BENAME}-${be_idx}"; do
		be_idx=$((be_idx + 1))
	done
	BENAME="${BENAME}-${be_idx}"
fi

## Create and mount the new boot environment.
BE_TMP=$(mktemp -d /tmp/bepkginstall.XXXXXX) ||
	errx "Unable to create mountpoint"
[ -z "$NO_CLEANUP_BE" ] && cleanup_commands="rmdir_be ${cleanup_commands}"
BE_MNTPT="${BE_TMP}/mnt"
mkdir -p "${BE_MNTPT}"

${BE_UTILITY} create "${BENAME}" >/dev/null || errx "Unable to create BE ${BENAME}"
[ -z "$NO_CLEANUP_BE" ] && cleanup_commands="cleanup_be ${cleanup_commands}"

${BE_UTILITY} mount "${BENAME}" "${BE_TMP}/mnt" || errx "Unable to mount BE ${BENAME}"

echo "Mounted ${BENAME} to ${BE_MNTPT}"

## Configure a temporary local repository in the BE.
# Resolve the repo path to an absolute path for use inside the chroot.
real_repo=$(realpath "${local_repo}")

mkdir -p "${BE_MNTPT}/etc/pkg"
cleanup_commands="cleanup_repo_conf ${cleanup_commands}"

cat > "${BE_MNTPT}/etc/pkg/FreeBSD-local-base.conf" <<EOF
FreeBSD-local-base: {
    url: "file://${real_repo}",
    enabled: true,
    signature_type: "none",
    priority: 100
}
EOF

## Mount necessary filesystems into the BE for chroot operation.
# Track created directories so cleanup_be can remove them.
create_be_dirs "${real_repo}" || errx "Unable to create BE dirs"
mount -t nullfs "${real_repo}" "${BE_MNTPT}${real_repo}" || \
	errx "Unable to nullfs mount repo into BE"

mount -t devfs devfs "${BE_MNTPT}/dev" || errx "Unable to mount devfs"

echo "Upgrading base system packages ..."
BE_PKG="chroot ${BE_MNTPT} env ASSUME_ALWAYS_YES=true pkg -o IGNORE_OSVERSION=yes -o ABI=${pkg_abi}"

${BE_PKG} update -r FreeBSD-local-base || \
	errx "Unable to update pkg catalogue"
# Build the package list from the local repo, selecting only the kernel
# that matches KERNCONF.  Other kernel configs are excluded to prevent
# e.g., GENERIC from overwriting GENERIC-DEBUG in /boot/kernel/.
# The FreeBSD-set-kernels meta-package is also excluded as it depends
# on all kernel configs and would pull in unwanted kernels.
install_pkgs=$(${BE_PKG} rquery -r FreeBSD-local-base '%n' | awk \
    -v kern="FreeBSD-kernel-${kern_pkg_name}" '
    /^FreeBSD-set-kernels/ { next }
    /^FreeBSD-kernel-/ && !/^FreeBSD-kernel-man/ {
        pkg = $0; sub(/-dbg$/, "", pkg)
        if (pkg == kern) print $0
        next
    }
    { print }
')
[ -z "${install_pkgs}" ] && errx "No packages found to install"
# shellcheck disable=SC2086 # intentional word splitting
${BE_PKG} install -f -r FreeBSD-local-base -U ${install_pkgs} || \
	errx "Unable to upgrade base packages"

## Run config updater if configured.
if [ -n "${CONFIG_UPDATER}" ]; then
	echo "Running ${CONFIG_UPDATER} ..."
	"update_${CONFIG_UPDATER}" ||
		errx "${CONFIG_UPDATER} failed!"
fi

## Upgrade third-party packages if requested.
if [ -z "${NO_PKG_UPGRADE}" ]; then
	echo "Upgrading third-party packages ..."
	${BE_PKG} update || errx "Unable to update pkg"
	${BE_PKG} upgrade || errx "Unable to upgrade third-party packages"
fi

## Finalize.
cleanup_repo_conf

if [ -n "$NO_CLEANUP_BE" ]; then
	echo "Boot Environment ${BENAME} may be examined in ${BE_MNTPT}."
	echo "Afterwards, run this to cleanup:"
	echo "  env BENAME=${BENAME} BE_MNTPT=${BE_MNTPT} BEINSTALL_CMD=postmortem $0"
	exit 0
fi
unmount_be || errx "Unable to unmount BE"
rmdir_be || errx "Unable to cleanup BE"
${BE_UTILITY} activate "${BENAME}" || errx "Unable to activate BE"
echo
${BE_UTILITY} list
echo
echo "Boot environment ${BENAME} setup complete; reboot to use it."
