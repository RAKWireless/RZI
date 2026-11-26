# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0

zephyr_library_sources(
  lorawan.c
  lorawan_settings.c
  api/lorawan_config.c
  api/lorawan_network.c
  api/lorawan_channel.c
  api/lorawan_class_b.c
  api/lorawan_session.c
  api/lorawan_credentials.c
  api/lorawan_mac_commands.c
  api/lorawan_multicast.c
  api/lorawan_certification.c
)

zephyr_library_sources_ifdef(
  CONFIG_RZI_LORAWAN_BACKEND_USP
  backend/usp/lorawan_backend_usp.c
  backend/usp/lorawan_backend_usp_network.c
  backend/usp/lorawan_backend_usp_class_b.c
  backend/usp/lorawan_backend_usp_mac.c
  backend/usp/lorawan_backend_usp_multicast.c
  backend/usp/lorawan_backend_usp_session.c
)
if(CONFIG_RZI_LORAWAN_BACKEND_USP)
  zephyr_library_sources_ifdef(
    CONFIG_LORA_BASICS_MODEM_CERTIFICATION
    backend/usp/lorawan_backend_usp_certification.c
  )
  include(${CMAKE_CURRENT_LIST_DIR}/backend/usp/lbm_checks.cmake)
endif()
zephyr_library_sources_ifdef(
  CONFIG_RZI_LORAWAN_BACKEND_ZEPHYR
  backend/zephyr/lorawan_backend_zephyr.c
  backend/zephyr/lorawan_backend_zephyr_features.c
)

zephyr_library_sources_ifdef(CONFIG_RZI_LORAWAN_FUOTA
  services/fuota/lorawan_fuota.c
  services/fuota/fuota_header.c
  services/fuota/fuota_install.c)

if(CONFIG_RZI_LORAWAN_FUOTA_DIFF)
  set(RZI_HPATCHLITE_DIR "${ZEPHYR_BASE}/../modules/HPatchLite")
  set(RZI_TUZ_DEC_C "${RZI_HPATCHLITE_DIR}/tinyuz/decompress/tuz_dec.c")
  zephyr_library_sources(
    services/fuota/fuota_diff.c
    ${RZI_HPATCHLITE_DIR}/HDiffPatch/libHDiffPatch/HPatchLite/hpatch_lite.c
    ${RZI_TUZ_DEC_C}
  )
  zephyr_library_include_directories(
    ${RZI_HPATCHLITE_DIR}/HDiffPatch/libHDiffPatch/HPatchLite
    ${RZI_HPATCHLITE_DIR}/tinyuz/decompress
  )
  set_source_files_properties(${RZI_TUZ_DEC_C} PROPERTIES
    COMPILE_DEFINITIONS "_IS_USED_SHARE_hpatch_lite_types=1"
  )
endif()

if(CONFIG_RZI_LORAWAN_BACKEND_ZEPHYR)
  set(RZI_ZEPHYR_MIB_C "${CMAKE_CURRENT_LIST_DIR}/backend/zephyr/lorawan_zephyr_mib.c")
  cmake_language(EVAL CODE
    "cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" CALL target_sources loramac-node PRIVATE \"${RZI_ZEPHYR_MIB_C}\")"
  )
endif()
