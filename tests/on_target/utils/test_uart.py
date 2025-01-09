##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import time
from unittest.mock import Mock, patch

import pytest
from uart import Uart


def counter():
    i = 0
    while True:
        yield i
        i += 1

@patch("uart.Uart.__init__")
def mocked_uart(uart_init):
    uart_init.return_value = None
    u = Uart("uart")
    u._evt = Mock()
    u._evt.is_set.return_value = False
    return u

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_1(time_sleep, time_time):
    """Test that wait_for_str() works for list"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    u.wait_for_str(["foo", "bar", "baz"])

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_2_out_of_order(time_sleep, time_time):
    """Test that wait_for_str() works for out of order list"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    u.wait_for_str(["baz", "foo", "bar"], timeout=3)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_3_missing(time_sleep, time_time):
    """Test that wait_for_str() asserts when string is missing"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    with pytest.raises(AssertionError) as ex_info:
        u.wait_for_str(["baz", "foo", "bar", "1234"], timeout=3)
    assert "1234" in str(ex_info.value)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_ordered_1(time_sleep, time_time):
    """Test that wait_for_str_ordered() works for list"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    u.wait_for_str_ordered(["foo", "bar", "baz"], timeout=3)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_ordered_2_missing(time_sleep, time_time):
    """Test that wait_for_str_ordered() asserts when string is missing"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    with pytest.raises(AssertionError) as ex_info:
        u.wait_for_str_ordered(["foo", "bar", "baz", "1234"], timeout=3)
    assert "1234 missing" in str(ex_info.value)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_ordered_3_out_of_order(time_sleep, time_time):
    """Test that wait_for_str_ordered() asserts when strings are out of order"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    with pytest.raises(AssertionError) as ex_info:
        u.wait_for_str_ordered(["foo", "baz", "bar"], timeout=3)
    assert "bar missing" in str(ex_info.value)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_ordered_4_multiple(time_sleep, time_time):
    """Test that wait_for_str_ordered() works for multiple identical strings"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\nfoo123\nfoo123\nbar123\n"
    u.wait_for_str_ordered(["foo", "foo", "foo"], timeout=3)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_ordered_5_overflow(time_sleep, time_time):
    """Test that wait_for_str_ordered() asserts when too many identical strings"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\nfoo123\nfoo123\nbar123\n"
    with pytest.raises(AssertionError) as ex_info:
        u.wait_for_str_ordered(["foo", "foo", "foo", "foo"], timeout=3)
    assert "foo missing" in str(ex_info.value)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_ordered_6_out_of_order(time_sleep, time_time):
    """Test that wait_for_str_ordered() asserts when strings are out of order"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\nfoo123\nfoo123\nbar123\n"
    with pytest.raises(AssertionError) as ex_info:
        u.wait_for_str_ordered(["foo", "bar", "foo", "baz"], timeout=3)
    assert "baz missing" in str(ex_info.value)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_ordered_7_none(time_sleep, time_time):
    """Test that wait_for_str_ordered() asserts when no strings are found"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\nfoo123\nfoo123\nbar123\n"
    with pytest.raises(AssertionError) as ex_info:
        u.wait_for_str_ordered(["abc", "def", "ghi", "jkl"], timeout=2)
    assert "abc missing" in str(ex_info.value)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_8_get_current_size(time_sleep, time_time):
    """Test that wait_for_str() returns current log size"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    current_log_size = u.wait_for_str(["bar"], timeout=3)
    assert current_log_size == len(u.log)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_9_start_position(time_sleep, time_time):
    """Test that wait_for_str() starts from given position"""
    u = mocked_uart()
    u.log = "foo123\nbar123\nbaz123\n"
    bar_pos = u.log.find("bar")
    with pytest.raises(AssertionError) as ex_info:
        u.wait_for_str(["baz", "foo", "bar"], timeout=3, start_pos=bar_pos)
    assert "foo" in str(ex_info.value)
    u.wait_for_str(["bar", "baz"], timeout=3, start_pos=bar_pos)

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_10_extract_one_value(time_sleep, time_time):
    """Test that extract_value() works for one value"""
    u = mocked_uart()
    u.log = "foo: 123.45\n bar: 23.45 \n  baz: 0.1234\n"
    assert float(u.extract_value(r"bar: (\d.+)")[0]) == 23.45

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_11_extract_three_values(time_sleep, time_time):
    """Test that extract_value() works for multiple values"""
    u = mocked_uart()
    u.log = "foo: 123.45 bar: 23.45  baz: 0.1234"
    extrated_values = u.extract_value(r"foo: (\d.+) bar: (\d.+) baz: (\d.+)")
    assert [float(x) for x in extrated_values] == [123.45, 23.45, 0.1234]

@patch("time.time", side_effect=counter())
@patch("time.sleep")
def test_wait_12_extract_missing_values(time_sleep, time_time):
    """Test that extract_value() returns None when values are missing"""
    u = mocked_uart()
    u.log = "foo: 123.45 baz: 23.45  bar: 0.1234"
    extrated_values = u.extract_value(r"foo: (\d.+) foo: (\d.+) foo: (\d.+)")
    assert extrated_values is None
