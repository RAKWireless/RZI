# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0

set(RZI_LBM_CORE_DIR ${ZEPHYR_USP_MODULE_DIR}/protocols/lbm_lib/smtc_modem_core)
set(RZI_LBM_SE_DIR ${RZI_LBM_CORE_DIR}/smtc_modem_crypto/smtc_secure_element)
zephyr_library_include_directories(${RZI_LBM_SE_DIR})
zephyr_library_include_directories(${RZI_LBM_CORE_DIR}/lr1mac/src
                                  ${RZI_LBM_CORE_DIR}/lr1mac/src/services
                                  ${ZEPHYR_USP_MODULE_DIR}/smtc_rac_lib/smtc_ral/src)
if(CONFIG_RZI_LORAWAN_FUOTA_AGENT)
  zephyr_library_include_directories(${RZI_LBM_CORE_DIR}/lorawan_packages/fragmented_data_block_transport)
  if(NOT EXISTS ${RZI_LBM_CORE_DIR}/lorawan_packages/fragmented_data_block_transport/lorawan_fuota_observer.h)
    message(FATAL_ERROR "The FUOTA receive agent requires the USP v1 FUOTA observer patch")
  endif()
endif()

# lorawan_backend_usp_lbm.h repeats these prototypes; keep them in sync with LBM.
function(rzi_check_lbm_prototype header regex)
  set(path ${RZI_LBM_CORE_DIR}/${header})
  if(NOT EXISTS ${path})
    message(FATAL_ERROR "LBM header not found: ${path}")
  endif()
  file(STRINGS ${path} found REGEX "${regex}")
  if(NOT found)
    message(FATAL_ERROR "LBM prototype changed in ${header}: expected '${regex}'. "
                        "Update rzi/src/lorawan/backend/usp/lorawan_backend_usp_lbm.h.")
  endif()
endfunction()

rzi_check_lbm_prototype(lorawan_api/lorawan_api.h
  "^uint32_t lorawan_api_devaddr_get\\( uint8_t stack_id \\);")
rzi_check_lbm_prototype(lorawan_api/lorawan_api.h
  "^uint8_t lorawan_api_next_dr_get\\( uint8_t stack_id \\);")
rzi_check_lbm_prototype(lorawan_api/lorawan_api.h
  "^uint32_t lorawan_api_next_join_time_second_get\\( uint8_t stack_id \\);")
rzi_check_lbm_prototype(lorawan_api/lorawan_api.h
  "^void lorawan_api_set_no_rx_packet_threshold\\( uint16_t no_rx_packet_reset_threshold, uint8_t stack_id \\);")
rzi_check_lbm_prototype(lorawan_api/lorawan_api.h
  "^uint16_t lorawan_api_get_no_rx_packet_threshold\\( uint8_t stack_id \\);")
rzi_check_lbm_prototype(lorawan_api/lorawan_api.h
  "^void lorawan_api_set_activation_mode\\( lr1mac_activation_mode_t activation_mode, uint8_t stack_id \\);")
rzi_check_lbm_prototype(lr1mac/src/services/smtc_duty_cycle.h
  "^smtc_dtc_enablement_type_t smtc_duty_cycle_enable_get\\( void \\);")
rzi_check_lbm_prototype(lorawan_packages/fragmented_data_block_transport/lorawan_fragmentation_package.h
  "^void lorawan_fragmentation_package_get_file_size\\( uint8_t stack_id, uint32_t\\* file_size \\);")
rzi_check_lbm_prototype(smtc_modem_crypto/smtc_secure_element/smtc_secure_element.h
  "^smtc_se_return_code_t smtc_secure_element_get_key\\( smtc_se_key_identifier_t key_id")

file(STRINGS ${RZI_LBM_CORE_DIR}/../smtc_modem_api/smtc_modem_api.h found
     REGEX "^smtc_modem_return_code_t smtc_modem_set_channel_mask\\(")
if(NOT found)
  message(FATAL_ERROR "smtc_modem_set_channel_mask() is missing from USP. "
                      "Apply the RZI patches: west patch -sm rzi apply (see rzi/doc/west-patch.md).")
endif()
