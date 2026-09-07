# SPDX-License-Identifier: BSD-2-Clause

rssmap_binary()
{

	if atf_config_has rssmap; then
		atf_config_get rssmap
	elif [ -n "${RSSMAP:-}" ]; then
		printf '%s\n' "${RSSMAP}"
	else
		command -v rssmap || atf_fail 'Set RSSMAP or the rssmap test variable'
	fi
}

run_fixture()
{
	fixture_case=$1
	shift

	atf_check -s exit:0 -o save:output -e empty \
	    env LD_PRELOAD="$(atf_get_srcdir)/rssmap_fixture.so" \
	    RSSMAP_TEST_CASE="${fixture_case}" "$(rssmap_binary)" "$@" lo0
}

check_row()
{

	# shellcheck disable=SC2016
	atf_check -s exit:0 -o save:normalized -e empty \
	    awk '{$1 = $1; print}' output
	atf_check -s exit:0 -o ignore -e empty \
	    grep -Fx -- "$1" normalized
}

check_count()
{

	atf_check -s exit:0 -o "inline:$1\n" -e empty awk \
	    '/^[[:space:]]*[0-9]+[[:space:]]/ { n++ } END { print n + 0 }' output
}

atf_test_case summary
summary_body()
{
	run_fixture basic
	check_row '0 0 2 1 1 0 0,4'
	check_row '1 2 2 1 1 0 2,6'
	check_count 2
}

atf_test_case detail
detail_body()
{
	run_fixture basic -b
	check_row '0 0 0 0 0 0 same'
	check_row '1 1 1 2 1 2 same'
	check_row '2 2 0 0 2 4 different'
	check_row '3 3 1 2 3 6 different'
	check_count 4
}

atf_test_case unequal_tables
unequal_tables_body()
{
	run_fixture software_larger
	check_row '0 0 2 1 3 0 0,4,8,12'
	run_fixture software_larger -b
	check_row '4 0 0 0 4 8 different'
	check_row '7 3 1 2 7 14 different'
	check_count 8
	run_fixture hardware_larger
	check_row '0 0 4 2 2 0 0,4'
	run_fixture hardware_larger -b
	check_row '4 4 0 0 0 0 same'
	check_row '7 7 1 2 3 6 different'
	check_count 8
}

atf_test_case renamed_padded
renamed_padded_body()
{
	for count in 10 11 12 100 101; do
		run_fixture "queues${count}" -b
		last=$((count - 1))
		cpu=$((last * 2))
		bucket=$((last % 4))
		rss_cpu=$((bucket * 2))
		check_row "${last} ${last} ${last} ${cpu} ${bucket} ${rss_cpu} different"
	done
}

atf_test_case missing_cpu
missing_cpu_body()
{
	run_fixture cpu_unreadable
	check_row '1 - 2 0 0 2 2,6'
	run_fixture cpu_unreadable -b
	check_row '1 1 1 - 1 2 unknown'
}

atf_test_case no_rss
no_rss_body()
{
	run_fixture no_rss
	check_row '0 0 2'
	check_row '1 2 2'
	check_count 2
	atf_check -s exit:0 -o ignore -e empty \
	    grep -Fx 'software RSS: disabled' output
	run_fixture no_rss -b
	check_row '3 3 1 2'
	check_count 4
}

atf_test_case cpu_ranges
cpu_ranges_body()
{
	run_fixture cpu_ranges
	check_row '0 0 2 0 2 0 2-3'
	check_row '1 2 2 0 2 0 8'
	run_fixture unused_queue
	check_row '2 4 0 0 0 0 -'
	check_count 3
}

atf_test_case netisr_state
netisr_state_body()
{
	for dispatch in direct hybrid deferred; do
		run_fixture "${dispatch}"
		atf_check -s exit:0 -o ignore -e empty \
		    grep -Fx "netisr: ${dispatch}, 8 workers, bound" output
	done
	run_fixture one_worker
	atf_check -s exit:0 -o ignore -e empty \
	    grep -Fx 'netisr: direct, 1 worker, bound' output
	run_fixture unbound
	atf_check -s exit:0 -o ignore -e empty \
	    grep -Fx 'netisr: direct, 8 workers, unbound' output
}

atf_test_case invalid_table
invalid_table_body()
{
	atf_check -s exit:1 -o empty -e match:'SIOCGIFRSSTABLE' \
	    env LD_PRELOAD="$(atf_get_srcdir)/rssmap_fixture.so" \
	    RSSMAP_TEST_CASE=no_table "$(rssmap_binary)" lo0
	for bad in table_zero table_oversize table_nonpower queues_zero \
	    queue_out_of_range; do
		atf_check -s exit:1 -o ignore -e match:'invalid RSS table' \
		    env LD_PRELOAD="$(atf_get_srcdir)/rssmap_fixture.so" \
		    RSSMAP_TEST_CASE="${bad}" "$(rssmap_binary)" lo0
	done
}

atf_test_case invalid_map
invalid_map_body()
{
	for bad in map_duplicate map_missing map_out_of_range map_negative \
	    map_junk; do
		atf_check -s exit:1 -o ignore \
		    -e match:'invalid RSS bucket mapping' \
		    env LD_PRELOAD="$(atf_get_srcdir)/rssmap_fixture.so" \
		    RSSMAP_TEST_CASE="${bad}" "$(rssmap_binary)" lo0
	done
	for bad in bucket_zero bucket_nonpower; do
		atf_check -s exit:1 -o ignore -e match:'invalid RSS bucket count' \
		    env LD_PRELOAD="$(atf_get_srcdir)/rssmap_fixture.so" \
		    RSSMAP_TEST_CASE="${bad}" "$(rssmap_binary)" lo0
	done
}

atf_test_case optional_data
optional_data_body()
{
	for missing in key_unreadable software_key_unreadable; do
		run_fixture "${missing}"
		check_row '0 0 2 1 1 0 0,4'
		atf_check -s exit:0 -o ignore -e empty \
		    grep -E '^hash:.*key comparison unavailable$' output
	done
	run_fixture hash_unreadable
	check_row '0 0 2 1 1 0 0,4'
	atf_check -s exit:0 -o ignore -e empty \
	    grep -E '^hash: unavailable' output
	run_fixture name_unreadable
	check_row '0 - 2 0 0 2 0,4'
	for missing in map_unreadable bucket_unreadable feature_unreadable; do
		run_fixture "${missing}"
		check_row '0 0 2'
		atf_check -s exit:0 -o ignore -e empty \
		    grep -E '^software RSS: unavailable' output
	done
	run_fixture netisr_unreadable
	atf_check -s exit:0 -o ignore -e empty \
	    grep -Fx 'netisr: unavailable' output
	run_fixture different_key
	check_row '0 0 2 1 1 0 0,4'
	atf_check -s exit:0 -o ignore -e empty \
	    grep -E '^hash:.*key comparison different$' output
}

atf_init_test_cases()
{
	atf_add_test_case summary
	atf_add_test_case detail
	atf_add_test_case unequal_tables
	atf_add_test_case renamed_padded
	atf_add_test_case missing_cpu
	atf_add_test_case no_rss
	atf_add_test_case cpu_ranges
	atf_add_test_case netisr_state
	atf_add_test_case invalid_table
	atf_add_test_case invalid_map
	atf_add_test_case optional_data
}
