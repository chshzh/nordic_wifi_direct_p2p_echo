#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wifi_direct_p2p_echo)

# Source files
target_sources(app PRIVATE
    src/main.c
    src/wifi_p2p_utils.c
    src/net_utils.c
    src/udp_utils.c
)
