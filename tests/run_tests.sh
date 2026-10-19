#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Unit tests on this computer: the bytebeat compiler and interpreter (tests/test_bytebeat.c).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O2 -Wall -Wextra -Wno-parentheses -fwrapv -Ifirmware/src -o build/host/test_bytebeat tests/test_bytebeat.c
build/host/test_bytebeat
