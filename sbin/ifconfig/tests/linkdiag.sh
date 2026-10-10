# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Nick Price <nprice@FreeBSD.org>

mock_env()
{
	LD_PRELOAD="$(atf_get_srcdir)/linkdiag_mock.so"
	LINKDIAG_MOCK_MODE=$1
	export LD_PRELOAD LINKDIAG_MOCK_MODE
}

expected_pairs()
{
	cat >expected <<EOF
pair A: open at 2 m (far end 2 m)
pair B: open at 2 m (far end 2 m)
pair C: short at 1 m (far end 2 m)
pair D: crossed at 1 m (far end 2 m)
EOF
}

atf_test_case cable_results
cable_results_head()
{
	atf_set descr "Print cable faults and distances through linkdiag"
}
cable_results_body()
{
	expected_pairs
	mock_env screenshot
	atf_check -s exit:0 -o file:expected -e empty \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
	mock_env empty-sfp
	atf_check -s exit:0 -o file:expected -e empty \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
}

atf_test_case fractional_distances
fractional_distances_head()
{
	atf_set descr "Preserve centimetres and valid zero distances"
}
fractional_distances_body()
{
	cat >expected <<EOF
pair A: open at 2.05 m (far end 2 m)
pair B: open at 2.1 m (far end 2 m)
pair C: short at 0 m (far end 2 m)
pair D: crossed at 42949672.95 m (far end 2 m)
EOF
	mock_env fractional
	atf_check -s exit:0 -o file:expected -e empty \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
}

atf_test_case pair_statuses
pair_statuses_head()
{
	atf_set descr "Handle unknown results, absent distances, and peer bounds"
}
pair_statuses_body()
{
	cat >expected <<EOF
pair A: OK
pair B: unknown
pair C: low impedance mismatch
pair D: high impedance mismatch
EOF
	mock_env statuses
	atf_check -s exit:0 -o file:expected -e empty \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
	cat >expected <<EOF
pair A: busy
pair B: failed
pair C: cross-pair short with pair A
pair D: crossed
EOF
	mock_env peer-statuses
	atf_check -s exit:0 -o file:expected -e empty \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
}

atf_test_case cached_results
cached_results_head()
{
	atf_set descr "Verbose status reads cached results without starting tests"
}
cached_results_body()
{
	mock_env cached
	atf_check -s exit:0 -o save:status -e empty \
	    "$(atf_config_get ifconfig ifconfig)" -v lo0
	atf_check -o match:'linkdiag:' grep linkdiag status
	atf_check -o match:'pair A: open at 2 m' grep 'pair A:' status
	mock_env uncached
	atf_check -s exit:0 -o save:status -e empty \
	    "$(atf_config_get ifconfig ifconfig)" -v lo0
	atf_check -s exit:1 -o empty grep linkdiag status
	mock_env unsupported
	atf_check -s exit:0 -o save:status -e empty \
	    "$(atf_config_get ifconfig ifconfig)" -v lo0
	atf_check -s exit:1 -o empty grep linkdiag status
}

atf_test_case invalid_results
invalid_results_head()
{
	atf_set descr "Reject malformed versions and pair counts"
}
invalid_results_body()
{
	for mode in bad-version bad-count zero-count; do
		mock_env "${mode}"
		atf_check -s exit:1 -o empty -e match:'Protocol error' \
		    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
		atf_check -s exit:0 -o save:status -e empty \
		    "$(atf_config_get ifconfig ifconfig)" -v lo0
		atf_check -s exit:1 -o empty grep linkdiag status
	done
	for mode in bad-run-version bad-run-count zero-run-count; do
		mock_env "${mode}"
		atf_check -s exit:1 -o empty -e match:'Protocol error' \
		    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
	done
	mock_env incomplete
	atf_check -s exit:1 -o empty -e match:'no test result' \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
}

atf_test_case ioctl_errors
ioctl_errors_head()
{
	atf_set descr "Report unsupported diagnostics and permission errors"
}
ioctl_errors_body()
{
	mock_env unsupported
	atf_check -s exit:1 -o empty -e match:'Operation not supported' \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
	mock_env denied
	atf_check -s exit:1 -o empty -e match:'Operation not permitted' \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
}

atf_test_case disruptive_warning
disruptive_warning_head()
{
	atf_set descr "Warn before a disruptive cable test"
}
disruptive_warning_body()
{
	expected_pairs
	mock_env disruptive
	atf_check -s exit:0 -o file:expected -e match:'interrupts traffic' \
	    "$(atf_config_get ifconfig ifconfig)" lo0 linkdiag
}

atf_test_case optical_module
optical_module_head()
{
	atf_set descr "Read SFP diagnostics without cable tests or EEPROM dumps"
}
optical_module_body()
{
	for mode in sfp sfp-legacy; do
		mock_env "${mode}"
		atf_check -s exit:0 -o save:status -e empty \
		    "$(atf_config_get ifconfig ifconfig)" -vvvv lo0 linkdiag
		atf_check -o match:'vendor: Test optics' grep vendor: status
		atf_check -o match:'temperature: 25.00 C voltage: 3.30 Volts' \
		    grep 'module temperature:' status
		atf_check -o match:'RX power: 1.00 mW .* TX bias: 5.00 mA' \
		    grep 'lane 1:' status
		atf_check -s exit:1 -o empty grep -E 'pair|DUMP' status
	done
}

atf_init_test_cases()
{
	atf_add_test_case cable_results
	atf_add_test_case fractional_distances
	atf_add_test_case pair_statuses
	atf_add_test_case cached_results
	atf_add_test_case invalid_results
	atf_add_test_case ioctl_errors
	atf_add_test_case disruptive_warning
	atf_add_test_case optical_module
}
