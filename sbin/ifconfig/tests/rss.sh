# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Nick Price <nprice@FreeBSD.org>

. $(atf_get_srcdir)/../../sys/common/vnet.subr

atf_test_case "rss_status" "cleanup"
rss_status_head()
{
	atf_set descr "Verbose status is silent for an interface without RSS"
	atf_set require.user root
}
rss_status_body()
{
	local epair

	vnet_init

	epair=$(vnet_mkepair)
	atf_check -s exit:0 -o not-match:'rss' ifconfig -v ${epair}a
}
rss_status_cleanup()
{
	vnet_cleanup
}

atf_test_case "rsskey_syntax" "cleanup"
rsskey_syntax_head()
{
	atf_set descr "rsskey rejects malformed keys before any ioctl"
	atf_set require.user root
}
rsskey_syntax_body()
{
	local epair longkey

	vnet_init

	epair=$(vnet_mkepair)
	atf_check -s not-exit:0 -e match:'even number of hex digits' \
	    ifconfig ${epair}a rsskey abc
	atf_check -s not-exit:0 -e match:'even number of hex digits' \
	    ifconfig ${epair}a rsskey ''
	atf_check -s not-exit:0 -e match:'not a hex digit' \
	    ifconfig ${epair}a rsskey zz
	longkey=$(jot -b 0 -s '' 258)
	atf_check -s not-exit:0 -e match:'exceeds 128 bytes' \
	    ifconfig ${epair}a rsskey ${longkey}
}
rsskey_syntax_cleanup()
{
	vnet_cleanup
}

atf_test_case "rsstable_syntax" "cleanup"
rsstable_syntax_head()
{
	atf_set descr "rsstable rejects malformed queue lists before any ioctl"
	atf_set require.user root
}
rsstable_syntax_body()
{
	local epair

	vnet_init

	epair=$(vnet_mkepair)
	atf_check -s not-exit:0 -e match:'not a queue number' \
	    ifconfig ${epair}a rsstable q
	atf_check -s not-exit:0 -e match:'empty queue' \
	    ifconfig ${epair}a rsstable '0,,1'
	atf_check -s not-exit:0 -e match:'is reversed' \
	    ifconfig ${epair}a rsstable '3-1'
	atf_check -s not-exit:0 -e match:'trailing junk' \
	    ifconfig ${epair}a rsstable '0x'
	atf_check -s not-exit:0 -e match:'is too large' \
	    ifconfig ${epair}a rsstable 65536
	# strtoul(3) would take a sign, and a large negative wraps to a small
	# queue number.
	atf_check -s not-exit:0 -e match:'not a queue number' \
	    ifconfig ${epair}a rsstable -18446744073709551615
	atf_check -s not-exit:0 -e match:'not a queue number' \
	    ifconfig ${epair}a rsstable +1
	atf_check -s not-exit:0 -e match:'bad range' \
	    ifconfig ${epair}a rsstable '0--1'
}
rsstable_syntax_cleanup()
{
	vnet_cleanup
}

atf_test_case "rss_unsupported" "cleanup"
rss_unsupported_head()
{
	atf_set descr "A well formed request fails on an interface without RSS"
	atf_set require.user root
}
rss_unsupported_body()
{
	local epair

	vnet_init

	epair=$(vnet_mkepair)
	atf_check -s not-exit:0 -e match:'SIOCSIFRSSKEY' \
	    ifconfig ${epair}a rsskey 00112233
	atf_check -s not-exit:0 -e match:'SIOCGIFRSSTABLE' \
	    ifconfig ${epair}a rsstable 0
}
rss_unsupported_cleanup()
{
	vnet_cleanup
}

atf_test_case "rss_unprivileged"
rss_unprivileged_head()
{
	atf_set descr "An unprivileged caller may not program RSS"
	atf_set require.user root
	atf_set require.config unprivileged_user
}
rss_unprivileged_body()
{
	local user

	user=$(atf_config_get unprivileged_user)
	atf_check -s not-exit:0 -e match:'Operation not permitted' \
	    su -m ${user} -c 'ifconfig lo0 rsskey 00112233'
}

atf_init_test_cases()
{
	atf_add_test_case rss_status
	atf_add_test_case rsskey_syntax
	atf_add_test_case rsstable_syntax
	atf_add_test_case rss_unsupported
	atf_add_test_case rss_unprivileged
}
