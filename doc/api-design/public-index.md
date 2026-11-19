# Public service declarations

Status: normative

<a id="surface"></a>

These 11 headers declare 168 functions, counting getters/setters separately. Macros, enums, callback fields, private symbols, and the two AT headers are excluded. Counts include placeholders/conditional entries, not 168 supported operations.

| Service | Headers and counts | Total | Responsibility |
| --- | --- | --- | --- |
| Common | [init.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/init.h) (1), [version.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/version.h) (1), [err.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/err.h) (2) | 4 | Init, version, errors. |
| LoRaWAN | [lorawan.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/lorawan.h) (115), [multicast.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/multicast.h) (5), [certification.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/certification.h) (4), [fuota.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/fuota.h) (6) | 130 | Activation, traffic, link control, packages, updates. |
| Raw LoRa / FSK | [lora.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lora/lora.h) (18) | 18 | Configuration, traffic, tests. |
| Storage | [storage.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/storage/storage.h) (8) | 8 | Values, records, capacity. |
| Power | [power.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/power/power.h) (6) | 6 | PM switch, references, sleep. |
| Network Mode | [network_mode.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/network_mode.h) (2) | 2 | Persistent mode. |
| Total | 11 headers | 168 | Service/foundation surface. |

<a id="function-index"></a>

## Function index

Expand each header for names; declarations define full parameters, returns, and versions.

<details>
<summary>init.h · 1 functions</summary>

```text
rzi_init()
```

</details>

<details>
<summary>version.h · 1 functions</summary>

```text
rzi_version_get_string()
```

</details>

<details>
<summary>err.h · 2 functions</summary>

```text
rzi_err_str()
rzi_err_from_errno()
```

</details>

<details>
<summary>lorawan/lorawan.h · 115 functions</summary>

```text
rzi_lorawan_register_callbacks()
rzi_lorawan_unregister_callbacks()
rzi_lorawan_set_region()
rzi_lorawan_set_join_backoff_bypass()
rzi_lorawan_init()
rzi_lorawan_stop()
rzi_lorawan_deinit()
rzi_lorawan_get_state()
rzi_lorawan_get_confirmed()
rzi_lorawan_set_confirmed()
rzi_lorawan_get_retries()
rzi_lorawan_set_retries()
rzi_lorawan_set_net_id()
rzi_lorawan_clear_settings()
rzi_lorawan_get_dev_eui()
rzi_lorawan_set_dev_eui()
rzi_lorawan_get_app_eui()
rzi_lorawan_set_app_eui()
rzi_lorawan_get_app_key()
rzi_lorawan_set_app_key()
rzi_lorawan_get_gen_app_key()
rzi_lorawan_set_gen_app_key()
rzi_lorawan_get_dev_addr()
rzi_lorawan_set_dev_addr()
rzi_lorawan_get_nwk_skey()
rzi_lorawan_set_nwk_skey()
rzi_lorawan_get_app_skey()
rzi_lorawan_set_app_skey()
rzi_lorawan_get_activation()
rzi_lorawan_set_activation()
rzi_lorawan_get_auto_join()
rzi_lorawan_get_join_interval()
rzi_lorawan_get_join_attempts()
rzi_lorawan_get_join_backoff()
rzi_lorawan_join()
rzi_lorawan_leave()
rzi_lorawan_send()
rzi_lorawan_get_link_check_mode()
rzi_lorawan_request_link_check()
rzi_lorawan_get_device_time_enabled()
rzi_lorawan_request_device_time()
rzi_lorawan_get_network_time()
rzi_lorawan_set_class()
rzi_lorawan_is_joined()
rzi_lorawan_get_capabilities()
rzi_lorawan_get_region()
rzi_lorawan_get_class()
rzi_lorawan_get_adr()
rzi_lorawan_set_adr()
rzi_lorawan_get_data_rate()
rzi_lorawan_set_data_rate()
rzi_lorawan_get_tx_power()
rzi_lorawan_set_tx_power()
rzi_lorawan_get_duty_cycle()
rzi_lorawan_set_duty_cycle()
rzi_lorawan_get_rx1_delay()
rzi_lorawan_set_rx1_delay()
rzi_lorawan_get_rx2_delay()
rzi_lorawan_set_rx2_delay()
rzi_lorawan_get_rx2_data_rate()
rzi_lorawan_set_rx2_data_rate()
rzi_lorawan_get_rx2_frequency()
rzi_lorawan_set_rx2_frequency()
rzi_lorawan_get_join_accept_delay1()
rzi_lorawan_set_join_accept_delay1()
rzi_lorawan_get_join_accept_delay2()
rzi_lorawan_set_join_accept_delay2()
rzi_lorawan_get_public_network()
rzi_lorawan_set_public_network()
rzi_lorawan_get_lbt()
rzi_lorawan_set_lbt()
rzi_lorawan_get_lbt_rssi()
rzi_lorawan_set_lbt_rssi()
rzi_lorawan_get_lbt_scan_time()
rzi_lorawan_set_lbt_scan_time()
rzi_lorawan_get_adr_profile()
rzi_lorawan_set_adr_profile()
rzi_lorawan_get_nb_trans()
rzi_lorawan_set_nb_trans()
rzi_lorawan_get_adr_ack()
rzi_lorawan_set_adr_ack()
rzi_lorawan_set_join_data_rates()
rzi_lorawan_get_duty_cycle_status()
rzi_lorawan_get_csma()
rzi_lorawan_set_csma()
rzi_lorawan_get_csma_params()
rzi_lorawan_set_csma_params()
rzi_lorawan_get_lost_connection_count()
rzi_lorawan_get_no_downlink_threshold()
rzi_lorawan_set_no_downlink_threshold()
rzi_lorawan_get_crystal_error()
rzi_lorawan_set_crystal_error()
rzi_lorawan_send_empty()
rzi_lorawan_get_channel_mask()
rzi_lorawan_set_channel_mask()
rzi_lorawan_get_sub_band()
rzi_lorawan_set_sub_band()
rzi_lorawan_get_fixed_channel()
rzi_lorawan_set_fixed_channel()
rzi_lorawan_get_last_rssi()
rzi_lorawan_get_last_snr()
rzi_lorawan_get_protocol_version()
rzi_lorawan_get_net_id()
rzi_lorawan_get_dev_nonce()
rzi_lorawan_set_dev_nonce()
rzi_lorawan_query_tx_possible()
rzi_lorawan_is_busy()
rzi_lorawan_get_ping_slot_periodicity()
rzi_lorawan_set_ping_slot_periodicity()
rzi_lorawan_get_beacon_frequency()
rzi_lorawan_get_beacon_time()
rzi_lorawan_get_beacon_data_rate()
rzi_lorawan_get_beacon_gateway()
rzi_lorawan_get_class_b_state()
rzi_lorawan_stop_class_b()
```

</details>

<details>
<summary>lorawan/multicast.h · 5 functions</summary>

```text
rzi_lorawan_add_multicast_session()
rzi_lorawan_remove_multicast_session()
rzi_lorawan_get_multicast_count()
rzi_lorawan_get_multicast_session()
rzi_lorawan_clear_multicast_sessions()
```

</details>

<details>
<summary>lorawan/certification.h · 4 functions</summary>

```text
rzi_lorawan_get_certification_mode()
rzi_lorawan_set_certification_mode()
rzi_lorawan_get_certification_port_enabled()
rzi_lorawan_set_certification_port_enabled()
```

</details>

<details>
<summary>lorawan/fuota.h · 6 functions</summary>

```text
rzi_fuota_register_callbacks()
rzi_fuota_init()
rzi_fuota_get_status()
rzi_fuota_set_expected_size()
rzi_fuota_read_image()
rzi_fuota_apply()
```

</details>

<details>
<summary>lora/lora.h · 18 functions</summary>

```text
rzi_lora_register_callbacks()
rzi_lora_init()
rzi_lora_stop()
rzi_lora_deinit()
rzi_lora_is_active()
rzi_lora_get_config()
rzi_lora_set_config()
rzi_lora_send()
rzi_lora_receive()
rzi_lora_receive_active()
rzi_lora_poll()
rzi_lora_test_rssi()
rzi_lora_test_get_rssi()
rzi_lora_test_tone()
rzi_lora_test_tx()
rzi_lora_test_rx()
rzi_lora_test_cw()
rzi_lora_test_stop()
```

</details>

<details>
<summary>storage/storage.h · 8 functions</summary>

```text
rzi_storage_init()
rzi_storage_read()
rzi_storage_write()
rzi_storage_delete()
rzi_storage_get_info()
rzi_storage_get_usage()
rzi_storage_write_record()
rzi_storage_read_record()
```

</details>

<details>
<summary>power/power.h · 6 functions</summary>

```text
rzi_power_set_pm_enabled()
rzi_power_get_pm_enabled()
rzi_power_lock_get()
rzi_power_lock_put()
rzi_power_get_lock_mask()
rzi_power_sleep()
```

</details>

<details>
<summary>network_mode.h · 2 functions</summary>

```text
rzi_get_network_mode()
rzi_set_network_mode()
```

</details>

See also: [API design index](index.md).
