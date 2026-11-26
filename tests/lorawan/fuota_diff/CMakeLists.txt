# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rzi_lorawan_fuota_diff)

file(TO_CMAKE_PATH "${ZEPHYR_BASE}/../build/fuota-key-check/old.signed.bin" RZI_FUOTA_DIFF_OLD)
file(TO_CMAKE_PATH "${ZEPHYR_BASE}/../build/fuota-key-check/new.signed.bin" RZI_FUOTA_DIFF_NEW)
file(TO_CMAKE_PATH "${ZEPHYR_BASE}/../build/fuota-key-check/diff/firmware.bin" RZI_FUOTA_DIFF_PKG)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/lorawan
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/lorawan/services/fuota)
target_compile_definitions(app PRIVATE
  "RZI_FUOTA_DIFF_OLD=\"${RZI_FUOTA_DIFF_OLD}\""
  "RZI_FUOTA_DIFF_NEW=\"${RZI_FUOTA_DIFF_NEW}\""
  "RZI_FUOTA_DIFF_PKG=\"${RZI_FUOTA_DIFF_PKG}\""
)
target_sources(app PRIVATE src/main.c)
