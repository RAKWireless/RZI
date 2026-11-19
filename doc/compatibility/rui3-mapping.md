# RUI3 C service migration mapping

Status: normative

<a id="rui3-mapping"></a>

Migration reference only: RZI parameters, events, states, and support boundaries remain the contract.

The original local RUI3 review covered 183 application functions in seven component/service/lora/service_lora*.h headers. Stack internals, package callbacks, timers, and internal init were excluded. Conditional declarations were retained; duplicates were removed. This is not the whole SDK or all historical versions.

RZI columns use public parameter types/names and omit returns. Multiple targets are listed separately. A dash means no public destination.

Feature mapping does not promise signature, error, or timing compatibility. Mapped means a public API/event supplies the feature; Partial requires additional caller policy or covers less behavior; Unmapped has no equivalent public control; Declared, unavailable has a declaration without usable USP implementation. Categories are not a roadmap. Notes describe conditions and differences; original RUI3 spellings remain.

<a id="rui3-map-0"></a>

## 1 service_lora.h - 113 entries

<details>
<summary>service_lora.h mapping (113 entries)</summary>

| RUI3 C declaration | RZI public API / destination | Mapping | Differences and limits |
| --- | --- | --- | --- |
| `service_lora_init(SERVICE_LORA_BAND band)` | `rzi_lorawan_init(enum rzi_lorawan_region region)` | Mapped | Explicit region; READY reports initialization completion. |
| `service_lora_schedule_auto_join(void)` | `rzi_lorawan_join(int32_t start, int32_t auto_join, int32_t interval, int32_t attempts)` | Partial | start=1 begins joining, but no equivalent delayed auto-join timer is exposed. After READY, the application decides when to invoke join using saved policy. USP retries follow stack backoff. |
| `service_lora_get_band(void)` | `rzi_lorawan_get_region(enum rzi_lorawan_region *region)` | Mapped | RZI region enum; no EU433/LA915. Set only while inactive. |
| `service_lora_set_band(SERVICE_LORA_BAND band)` | `rzi_lorawan_set_region(enum rzi_lorawan_region region)` | Mapped | RZI region enum; no EU433/LA915. Set only while inactive. |
| `service_lora_get_mask(uint16_t *mask)` | `rzi_lorawan_get_channel_mask(uint16_t *mask, size_t words)` | Partial | Explicit word count; US915/AU915 only. Stored for the next join; no commit argument. |
| `service_lora_set_mask(uint16_t *mask, bool commit)` | `rzi_lorawan_set_channel_mask(const uint16_t *mask, size_t words)` | Partial | Explicit word count; US915/AU915 only. Stored for the next join; no commit argument. |
| `service_lora_get_app_eui(uint8_t *buff, uint32_t len)` | `rzi_lorawan_get_app_eui(uint8_t *eui, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_app_eui(uint8_t *buff, uint32_t len)` | `rzi_lorawan_set_app_eui(const uint8_t *eui, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_get_app_key(uint8_t *buff, uint32_t len)` | `rzi_lorawan_get_app_key(uint8_t *key, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_app_key(uint8_t *buff, uint32_t len)` | `rzi_lorawan_set_app_key(const uint8_t *key, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_get_app_skey(uint8_t *buff, uint32_t len)` | `rzi_lorawan_get_app_skey(uint8_t *key, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_app_skey(uint8_t *buff, uint32_t len)` | `rzi_lorawan_set_app_skey(const uint8_t *key, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_get_dev_addr(uint8_t *buff, uint32_t len)` | `rzi_lorawan_get_dev_addr(uint32_t *dev_addr)` | Mapped | A host-order uint32_t replaces the four-byte buffer. Reads the active address after OTAA join. |
| `service_lora_set_dev_addr(uint8_t *buff, uint32_t len)` | `rzi_lorawan_set_dev_addr(uint32_t dev_addr)` | Mapped | Host-order uint32_t replaces the four-byte buffer; saved for the next ABP activation. |
| `service_lora_get_dev_eui(uint8_t *buff, uint32_t len)` | `rzi_lorawan_get_dev_eui(uint8_t *eui, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_dev_eui(uint8_t *buff, uint32_t len)` | `rzi_lorawan_set_dev_eui(const uint8_t *eui, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_get_net_id(uint8_t *buff, uint32_t len)` | `rzi_lorawan_get_net_id(uint32_t *net_id)` | Partial | 24-bit NetID in uint32_t. Setter stores a fallback; getter prefers the active session where supported. |
| `service_lora_set_nwk_id(uint8_t *buff, uint32_t len)` | `rzi_lorawan_set_net_id(uint32_t net_id)` | Partial | 24-bit NetID in uint32_t. Setter stores a fallback; getter prefers the active session where supported. |
| `service_lora_get_nwk_skey(uint8_t *buff, uint32_t len)` | `rzi_lorawan_get_nwk_skey(uint8_t *key, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_nwk_skey(uint8_t *buff, uint32_t len)` | `rzi_lorawan_set_nwk_skey(const uint8_t *key, size_t len)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_get_McRoot_key(uint8_t *buff)` | — | Unmapped | No public McRootKey getter. GenAppKey does not replace this derived key. |
| `service_lora_set_lora_default(void)` | `rzi_lorawan_clear_settings(void)` | Partial | Clears service settings across keys without transaction guarantees. Reboot after success; not an immediate stack reset. |
| `service_lora_get_retry(void)` | `rzi_lorawan_get_retries(uint8_t *count)` | Partial | Stores policy 0-7; does not implement send retries or replace NbTrans. |
| `service_lora_set_retry(uint8_t retry)` | `rzi_lorawan_set_retries(uint8_t count)` | Partial | Stores policy 0-7; does not implement send retries or replace NbTrans. |
| `service_lora_get_cfm(void)` | `rzi_lorawan_get_confirmed(bool *enabled)` | Mapped | Stores the confirmation default; send still takes an explicit message type. |
| `service_lora_set_cfm(SERVICE_LORA_CONFIRM_MODE cfm)` | `rzi_lorawan_set_confirmed(bool enabled)` | Mapped | Stores the confirmation default; send still takes an explicit message type. |
| `service_lora_get_cfs(void)` | `rzi_lorawan_register_callbacks(const struct rzi_lorawan_callbacks *callbacks)` | Partial | No last-confirmation getter. Consume TXDONE status/error and retain results in the application. |
| `service_lora_join(int32_t param1, int32_t param2, int32_t param3, int32_t param4)` | `rzi_lorawan_join(int32_t start, int32_t auto_join, int32_t interval, int32_t attempts)` | Mapped | Keeps start/auto_join/interval/attempts; KEEP=-1. auto_join is stored policy; the application starts after READY. USP ignores interval in favor of stack backoff. |
| `service_lora_get_nwm(void)` | `rzi_get_network_mode(enum rzi_network_mode *mode)` | Mapped | Persists mode only; no implicit service start/stop or radio arbitration. |
| `service_lora_set_nwm(SERVICE_LORA_WORK_MODE nwm)` | `rzi_set_network_mode(enum rzi_network_mode mode)` | Mapped | Persists mode only; no implicit service start/stop or radio arbitration. |
| `service_lora_get_njm(void)` | `rzi_lorawan_get_activation(enum rzi_lorawan_activation *mode)` | Mapped | OTAA/ABP enum; no separate commit argument. |
| `service_lora_set_njm(SERVICE_LORA_JOIN_MODE njm, bool commit)` | `rzi_lorawan_set_activation(enum rzi_lorawan_activation mode)` | Mapped | OTAA/ABP enum; no separate commit argument. |
| `service_lora_get_njs(void)` | `rzi_lorawan_is_joined(bool *joined)` | Mapped | bool output separates the queried value from errors. |
| `service_lora_send(uint8_t *buff, uint32_t len, SERVICE_LORA_SEND_INFO info, bool blocking)` | `rzi_lorawan_send(uint8_t port, const uint8_t *data, size_t size, enum rzi_lorawan_message_type type)` | Mapped | Explicit port/data/size/type. No blocking argument; TXDONE reports completion. |
| `service_lora_get_adr(void)` | `rzi_lorawan_get_adr(bool *enabled)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_adr(bool adr, bool commit)` | `rzi_lorawan_set_adr(bool enabled)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_get_class(void)` | `rzi_lorawan_get_class(enum rzi_lorawan_class *device_class)` | Mapped | Class B/C require capabilities. Runtime settings do not promise RUI3 commit persistence. |
| `service_lora_get_real_class_from_stack(void)` | `rzi_lorawan_get_class(enum rzi_lorawan_class *device_class)` | Partial | Backend class read; no separate requested-class and actual-class query contract. |
| `service_lora_set_class(SERVICE_LORA_CLASS device_class, bool commit)` | `rzi_lorawan_set_class(enum rzi_lorawan_class device_class)` | Mapped | Class B/C require capabilities. Runtime settings do not promise RUI3 commit persistence. |
| `service_lora_get_dcs(void)` | `rzi_lorawan_get_duty_cycle(bool *enabled)` | Mapped | Implemented on USP, subject to state/region prerequisites. |
| `service_lora_set_dcs(uint8_t dutycycle, bool commit)` | `rzi_lorawan_set_duty_cycle(bool enabled)` | Mapped | Implemented on USP, subject to state/region prerequisites. |
| `service_lora_get_dr(void)` | `rzi_lorawan_get_data_rate(enum rzi_lorawan_data_rate *data_rate)` | Mapped | RZI data-rate enum; valid rates depend on region/backend. |
| `service_lora_set_dr(SERVICE_LORA_DATA_RATE dr, bool commit)` | `rzi_lorawan_set_data_rate(enum rzi_lorawan_data_rate data_rate)` | Mapped | RZI data-rate enum; valid rates depend on region/backend. |
| `service_lora_get_jn1dl(void)` | `rzi_lorawan_get_join_accept_delay1(uint32_t *delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_jn1dl(uint32_t jn1dl, bool commit)` | `rzi_lorawan_set_join_accept_delay1(uint32_t delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_get_jn2dl(void)` | `rzi_lorawan_get_join_accept_delay2(uint32_t *delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_jn2dl(uint32_t jn2dl, bool commit)` | `rzi_lorawan_set_join_accept_delay2(uint32_t delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_get_pub_nwk_mode(void)` | `rzi_lorawan_get_public_network(bool *enabled)` | Mapped | Corresponding USP network operation is assigned. |
| `service_lora_set_pub_nwk_mode(bool pnm, bool commit)` | `rzi_lorawan_set_public_network(bool enabled)` | Mapped | Corresponding USP network operation is assigned. |
| `service_lora_get_rx1dl(void)` | `rzi_lorawan_get_rx1_delay(uint32_t *delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_rx1dl(uint32_t rx1dl, bool commit)` | `rzi_lorawan_set_rx1_delay(uint32_t delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_get_rx2dl(void)` | `rzi_lorawan_get_rx2_delay(uint32_t *delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_rx2dl(uint32_t rx2dl, bool commit)` | `rzi_lorawan_set_rx2_delay(uint32_t delay_ms)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_get_txpower(void)` | `rzi_lorawan_get_tx_power(uint8_t *tx_power)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_txpower(uint8_t txp, bool commit)` | `rzi_lorawan_set_tx_power(uint8_t tx_power)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_get_ping_slot_periodicity(void)` | `rzi_lorawan_get_ping_slot_periodicity(uint8_t *periodicity)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_ping_slot_periodicity(uint8_t periodicity)` | `rzi_lorawan_set_ping_slot_periodicity(uint8_t periodicity)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_set_ping_slot_channel(uint32_t frequency, uint8_t datarate)` | — | Unmapped | No ping-slot frequency/data-rate control; periodicity is not a substitute. |
| `service_lora_get_ping_slot_channel(uint32_t *frequency, uint8_t *datarate)` | — | Unmapped | No ping-slot frequency/data-rate control; periodicity is not a substitute. |
| `service_lora_get_beacon_freq(void)` | `rzi_lorawan_get_beacon_frequency(uint32_t *frequency_hz)` | Declared, unavailable | USP class_b does not supply this query. |
| `service_lora_set_beacon_freq(uint32_t frequency)` | — | Unmapped | No public beacon-frequency setter. |
| `service_lora_get_beacon_time(void)` | `rzi_lorawan_get_beacon_time(uint32_t *gps_time)` | Declared, unavailable | No current USP beacon-time query. |
| `service_lora_get_rssi(void)` | `rzi_lorawan_get_last_rssi(int16_t *rssi_dbm)` | Mapped | RSSI of the latest application downlink, in dBm. |
| `service_lora_get_snr(void)` | `rzi_lorawan_get_last_snr(int8_t *snr_quarter_db)` | Mapped | SNR of the latest application downlink, in quarter-dB units. |
| `service_lora_get_local_time(char *local_time)` | `rzi_lorawan_get_network_time(struct rzi_lorawan_network_time *time)` | Mapped | GPS seconds/subseconds structure, not a formatted UTC string. |
| `service_lora_get_lorawan_version(char **version)` | `rzi_lorawan_get_protocol_version(const char **version)` | Mapped | Backend-owned static version string; no LmHandler types. |
| `service_lora_get_rx2freq(void)` | `rzi_lorawan_get_rx2_frequency(uint32_t *frequency_hz)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_rx2freq(uint32_t freq,bool commit)` | `rzi_lorawan_set_rx2_frequency(uint32_t frequency_hz)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_rx2dr(SERVICE_LORA_DATA_RATE datarate, bool commit)` | `rzi_lorawan_set_rx2_data_rate(enum rzi_lorawan_data_rate data_rate)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_get_rx2dr(void)` | `rzi_lorawan_get_rx2_data_rate(enum rzi_lorawan_data_rate *data_rate)` | Declared, unavailable | Declared, but the current facade returns NOT_SUPPORTED; readiness checks can fail first. |
| `service_lora_set_linkcheck(uint8_t mode)` | `rzi_lorawan_request_link_check(enum rzi_lorawan_link_check_mode mode)` | Mapped | Explicit mode with LINK_CHECK events. |
| `service_lora_get_linkcheck(void)` | `rzi_lorawan_get_link_check_mode(enum rzi_lorawan_link_check_mode *mode)` | Mapped | Explicit mode with LINK_CHECK events. |
| `service_lora_lptp_send(uint8_t port, bool ack, uint8_t *p_data, uint16_t len)` | — | Unmapped | No public LPTP fragmentation or dedicated completion callback; single-frame send is not equivalent. |
| `service_lora_get_join_start(void)` | `rzi_lorawan_get_state(enum rzi_lorawan_state *state)` | Partial | No join-start flag getter; observe JOINING through lifecycle state. |
| `service_lora_set_join_start(bool join_start)` | `rzi_lorawan_join(int32_t start, int32_t auto_join, int32_t interval, int32_t attempts)` | Mapped | start=1/0 starts/cancels joining; KEEP preserves other policy fields. |
| `service_lora_get_auto_join(void)` | `rzi_lorawan_get_auto_join(bool *enabled)` | Mapped | Reads saved policy; does not imply automatic joining by the service. |
| `service_lora_set_auto_join(bool auto_join)` | `rzi_lorawan_join(int32_t start, int32_t auto_join, int32_t interval, int32_t attempts)` | Mapped | Updates auto_join; start=KEEP changes policy only. |
| `service_lora_get_auto_join_period(void)` | `rzi_lorawan_get_join_interval(uint8_t *interval_s)` | Mapped | Interval in seconds; USP still uses stack backoff. |
| `service_lora_set_auto_join_period(uint32_t auto_join_period)` | `rzi_lorawan_join(int32_t start, int32_t auto_join, int32_t interval, int32_t attempts)` | Mapped | Updates interval; use the RZI join input range. |
| `service_lora_get_auto_join_max_cnt(void)` | `rzi_lorawan_get_join_attempts(uint8_t *attempts)` | Mapped | Attempts counts retries beyond the first; zero means one attempt. |
| `service_lora_set_auto_join_max_cnt(uint32_t auto_join_period)` | `rzi_lorawan_join(int32_t start, int32_t auto_join, int32_t interval, int32_t attempts)` | Mapped | Updates attempts; do not assume the old maximum-count meaning is identical. |
| `service_lora_get_last_recv(uint8_t *port, uint8_t *buff, uint32_t len)` | `rzi_lorawan_register_callbacks(const struct rzi_lorawan_callbacks *callbacks)` | Partial | No last-payload getter. DOWNDATA supplies port/data; copy in the callback for later reads. |
| `service_lora_set_che(uint8_t sub_band)` | `rzi_lorawan_set_sub_band(uint8_t sub_band)` | Partial | US915/AU915 only: zero selects all; 1-8 select eight narrowband channels and the associated wideband channel. The historical RUI3 review found no implementation of this declaration in service_lora*.c. |
| `service_lora_get_che(void)` | `rzi_lorawan_get_sub_band(uint8_t *sub_band)` | Partial | US915/AU915 only: zero selects all; 1-8 select eight narrowband channels and the associated wideband channel. The historical RUI3 review found no implementation of this declaration in service_lora*.c. |
| `service_lora_get_beacon_gwspecific(void)` | `rzi_lorawan_get_beacon_gateway(struct rzi_lorawan_beacon_gateway *gateway)` | Declared, unavailable | Public structure exists, but USP does not supply a beacon-gateway query. |
| `service_lora_get_chs(void)` | `rzi_lorawan_get_fixed_channel(uint32_t *frequency_hz)` | Partial | Fixed channel in Hz, expressed as a view of the stored channel mask. |
| `service_lora_set_chs(uint32_t frequency)` | `rzi_lorawan_set_fixed_channel(uint32_t frequency_hz)` | Partial | Fixed channel in Hz, expressed as a view of the stored channel mask. |
| `service_lora_register_recv_cb(service_lora_recv_cb callback)` | `rzi_lorawan_register_callbacks(const struct rzi_lorawan_callbacks *callbacks)` | Mapped | Unified on_event subscription; select DOWNDATA. Event types define data/error semantics. |
| `service_lora_register_join_cb(service_lora_join_cb callback)` | `rzi_lorawan_register_callbacks(const struct rzi_lorawan_callbacks *callbacks)` | Mapped | Unified on_event subscription; select JOINED/JOIN_FAILED. |
| `service_lora_register_send_cb(service_lora_send_cb callback)` | `rzi_lorawan_register_callbacks(const struct rzi_lorawan_callbacks *callbacks)` | Mapped | Unified on_event subscription; select TXDONE. |
| `service_lora_register_linkcheck_cb(service_lora_linkcheck_cb callback)` | `rzi_lorawan_register_callbacks(const struct rzi_lorawan_callbacks *callbacks)` | Mapped | Unified on_event subscription; select LINK_CHECK. |
| `service_lora_register_timereq_cb(service_lora_timereq_cb callback)` | `rzi_lorawan_register_callbacks(const struct rzi_lorawan_callbacks *callbacks)` | Mapped | Unified on_event subscription; select LORAWAN_MAC_TIME. |
| `service_lora_get_beacon_dr(void)` | `rzi_lorawan_get_beacon_data_rate(enum rzi_lorawan_data_rate *data_rate)` | Declared, unavailable | No current USP beacon-data-rate query. |
| `service_lora_get_class_b_state(void)` | `rzi_lorawan_get_class_b_state(enum rzi_lorawan_class_b_state *state)` | Mapped | Types/returns follow RZI declarations; see Chapter 4 for USP limits. |
| `service_lora_query_txPossible(int16_t len)` | `rzi_lorawan_query_tx_possible(size_t size)` | Mapped | Zero means the size fits the current data rate, not that a send was accepted. |
| `service_lora_suspend(void)` | — | Unmapped | RUI3 directly sleeps/wakes Radio. RZI delegates to service lifecycle/backend behavior; thread sleep and PM switches are not equivalent. |
| `service_lora_resume(void)` | — | Unmapped | RUI3 directly sleeps/wakes Radio. RZI delegates to service lifecycle/backend behavior; thread sleep and PM switches are not equivalent. |
| `service_lora_get_timereq(void)` | `rzi_lorawan_get_device_time_enabled(bool *enabled)` | Mapped | Enable/request DeviceTime; completion is LORAWAN_MAC_TIME. |
| `service_lora_set_timereq(uint8_t mode)` | `rzi_lorawan_request_device_time(bool enabled)` | Mapped | Enable/request DeviceTime; completion is LORAWAN_MAC_TIME. |
| `service_lora_systemMaxRxError(void)` | — | Unmapped | No public equivalent RX-window maximum-clock-error control; crystal_error has a different contract. |
| `service_lora_get_lbt()` | `rzi_lorawan_get_lbt(bool *enabled)` | Mapped | Corresponding USP network operation is assigned. |
| `service_lora_set_lbt(uint8_t enable)` | `rzi_lorawan_set_lbt(bool enabled)` | Mapped | Corresponding USP network operation is assigned. |
| `service_lora_get_lbt_rssi()` | `rzi_lorawan_get_lbt_rssi(int16_t *rssi_dbm)` | Mapped | Corresponding USP network operation is assigned. |
| `service_lora_set_lbt_rssi(int16_t rssi)` | `rzi_lorawan_set_lbt_rssi(int16_t rssi_dbm)` | Mapped | Corresponding USP network operation is assigned. |
| `service_lora_get_lbt_scantime()` | `rzi_lorawan_get_lbt_scan_time(uint32_t *time_ms)` | Mapped | Milliseconds; corresponding USP network operation is assigned. |
| `service_lora_set_lbt_scantime(uint32_t time)` | `rzi_lorawan_set_lbt_scan_time(uint32_t time_ms)` | Mapped | Milliseconds; corresponding USP network operation is assigned. |
| `service_lora_get_DevNonce(void)` | `rzi_lorawan_get_dev_nonce(uint16_t *dev_nonce)` | Declared, unavailable | Conditional LORA_STACK_104 declaration in RUI3. USP session ops do not supply it; public calls return NOT_SUPPORTED. |
| `service_lora_set_DevNonce(uint16_t devnonce)` | `rzi_lorawan_set_dev_nonce(uint16_t dev_nonce)` | Declared, unavailable | Conditional LORA_STACK_104 declaration in RUI3. USP session ops do not supply it; public calls return NOT_SUPPORTED. |
| `service_lora_set_IsCertPortOn(bool IsCertPortOn)` | `rzi_lorawan_set_certification_port_enabled(bool enabled)` | Mapped | Conditional LORA_STACK_104 declaration in RUI3. RZI USP maintains local certification-port state. |
| `service_lora_get_IsCertPortOn(void)` | `rzi_lorawan_get_certification_port_enabled(bool *enabled)` | Mapped | Conditional LORA_STACK_104 declaration in RUI3. RZI USP maintains local certification-port state. |
| `service_lora_isbusy(void)` | `rzi_lorawan_is_busy(bool *busy)` | Mapped | bool output; queries backend busy state only. |
| `service_lora_region_isActive(SERVICE_LORA_BAND band)` | — | Unmapped | No public compiled-region availability probe. |
| `service_lora_classb_force_stop(void)` | `rzi_lorawan_stop_class_b(void)` | Mapped | Requires USP Class B; switches to Class A and reports state. |

</details>

<a id="rui3-map-1"></a>

## 2 service_lora_p2p.h - 52 entries

<details>
<summary>service_lora_p2p.h mapping (52 entries)</summary>

| RUI3 C declaration | RZI public API / destination | Mapping | Differences and limits |
| --- | --- | --- | --- |
| `service_lora_p2p_get_nwm(void)` | `rzi_get_network_mode(enum rzi_network_mode *mode)` | Mapped | Persists mode; caller coordinates service lifecycle. |
| `service_lora_p2p_set_nwm(SERVICE_LORA_WORK_MODE nwm)` | `rzi_set_network_mode(enum rzi_network_mode mode)` | Mapped | Persists mode; caller coordinates service lifecycle. |
| `service_lora_p2p_init(void)` | `rzi_lora_init(enum rzi_lora_modulation modulation)` | Mapped | Explicit LoRa/FSK modulation. |
| `service_lora_p2p_config(void)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Complete configuration replaces applying scattered settings. |
| `service_lora_p2p_send(uint8_t *p_data, uint8_t len, bool cad_enable)` | `rzi_lora_send(const uint8_t *data, size_t size)` | Mapped | No cad_enable argument; CAD uses config.cad. |
| `service_lora_p2p_recv(uint32_t timeout)` | `rzi_lora_receive(uint32_t timeout_ms)` | Mapped | Milliseconds: zero stops reception; 65535 selects continuous reception. |
| `service_lora_p2p_get_freq(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.frequency_hz. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_freq(uint32_t freq)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.frequency_hz. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_check_runtime_freq(uint32_t freq)` | — | Unmapped | No standalone validation-only operation. set_config validates generic fields; backend constraints apply separately. |
| `service_lora_p2p_get_sf(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.spreading_factor. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_sf(uint8_t spreadfact)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.spreading_factor. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_check_runtime_sf(uint8_t spreadfact)` | — | Unmapped | No standalone validation-only operation. set_config validates generic fields; backend constraints apply separately. |
| `service_lora_p2p_get_bandwidth(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.bandwidth. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_bandwidth(uint32_t bandwidth)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.bandwidth. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_check_runtime_bandwidth(uint32_t bandwidth)` | — | Unmapped | No standalone validation-only operation. set_config validates generic fields; backend constraints apply separately. |
| `service_lora_p2p_get_codingrate(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.coding_rate. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_codingrate(uint8_t codingrate)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.coding_rate. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_check_runtime_codingrate(uint8_t codingrate)` | — | Unmapped | No standalone validation-only operation. set_config validates generic fields; backend constraints apply separately. |
| `service_lora_p2p_get_preamlen(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.preamble_length. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_preamlen(uint16_t preamlen)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.preamble_length. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_check_runtime_preamlen(uint16_t preamlen)` | — | Unmapped | No standalone validation-only operation. set_config validates generic fields; backend constraints apply separately. |
| `service_lora_p2p_get_powerdbm(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.tx_power_dbm. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_powerdbm(uint8_t powerdbm)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.tx_power_dbm. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_check_runtime_powerdbm(uint8_t powerdbm)` | — | Unmapped | No standalone validation-only operation. set_config validates generic fields; backend constraints apply separately. |
| `service_lora_p2p_get_crypto_enable(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Partial | Maps to rzi_lora_config.encrypt; current key/IV XOR is not RUI3 AES-CBC/PKCS#7. |
| `service_lora_p2p_set_crypto_enable(bool enable)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Partial | Maps to rzi_lora_config.encrypt; current key/IV XOR is not RUI3 AES-CBC/PKCS#7. |
| `service_lora_p2p_get_crypto_key(uint8_t *buff, uint32_t len)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Partial | Maps to rzi_lora_config.key; current key/IV XOR is not RUI3 AES-CBC/PKCS#7. |
| `service_lora_p2p_set_crypto_key(uint8_t *buff, uint32_t len)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Partial | Maps to rzi_lora_config.key; current key/IV XOR is not RUI3 AES-CBC/PKCS#7. |
| `service_lora_p2p_get_crypto_IV(uint8_t *buff, uint32_t len)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Partial | Maps to rzi_lora_config.iv; current key/IV XOR is not RUI3 AES-CBC/PKCS#7. |
| `service_lora_p2p_set_crypto_IV(uint8_t *buff, uint32_t len)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Partial | Maps to rzi_lora_config.iv; current key/IV XOR is not RUI3 AES-CBC/PKCS#7. |
| `service_lora_p2p_get_fdev(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.fsk_deviation. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_fdev(uint32_t fdev)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.fsk_deviation. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_get_bitrate(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.fsk_bitrate. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_bitrate(uint32_t bitrate)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.fsk_bitrate. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_encrpty(uint8_t *indata, uint16_t inlen,uint8_t *outdata)` | — | Unmapped | No public AES helper. Current XOR is not AES-CBC/PKCS#7. |
| `service_lora_p2p_decrpty(uint8_t *indata,uint16_t inlen ,uint8_t *outdata)` | — | Unmapped | No public AES helper. Current XOR is not AES-CBC/PKCS#7. |
| `service_lora_p2p_register_send_cb(service_lora_p2p_send_cb_type callback)` | `rzi_lora_register_callbacks(const struct rzi_lora_callbacks *callbacks)` | Mapped | tx_done plus user_data replaces separate registration. |
| `service_lora_p2p_register_send_CAD_cb(service_lora_p2p_send_CAD_cb_type callback)` | — | Unmapped | No separate CAD-send completion callback; ordinary tx_done is not equivalent. |
| `service_lora_p2p_register_recv_cb(service_lora_p2p_recv_cb_type callback)` | `rzi_lora_register_callbacks(const struct rzi_lora_callbacks *callbacks)` | Mapped | rx_done plus user_data; payload valid during the callback. |
| `service_lora_p2p_get_public_network(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Partial | config.sync_word expresses the sync word; no separate public-network bool. The historical RUI3 review found no implementation of this declaration in service_lora*.c. |
| `service_lora_p2p_set_public_network(bool enable)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Partial | config.sync_word expresses the sync word; no separate public-network bool. The historical RUI3 review found no implementation of this declaration in service_lora*.c. |
| `service_lora_p2p_get_symbol_timeout(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.symbol_timeout. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_symbol_timeout(uint32_t symbol_timeout)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.symbol_timeout. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_get_iqinverted(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.iq_inverted. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_iqinverted(bool iqinverted)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.iq_inverted. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_get_fix_length_payload(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.fixed_length. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_fix_length_payload(bool enable)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.fixed_length. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_get_syncword(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.sync_word. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_syncword(uint16_t syncword)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.sync_word. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_get_CAD(void)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.cad. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_set_CAD(bool enable)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Mapped | Maps to rzi_lora_config.cad. Use RZI field types/ranges rather than reusing the old layout. |
| `service_lora_p2p_get_radio_stat(void)` | — | Unmapped | No Radio.GetStatus equivalent. is_active/receive_active describe service state, not radio busy state. |

</details>

<a id="rui3-map-2"></a>

## 3 service_lora_multicast.h - 4 entries

<details>
<summary>service_lora_multicast.h mapping (4 entries)</summary>

| RUI3 C declaration | RZI public API / destination | Mapping | Differences and limits |
| --- | --- | --- | --- |
| `service_lora_addmulc(McSession_t McSession)` | `rzi_lorawan_add_multicast_session(const struct rzi_lorawan_multicast_session *session)` | Mapped | RZI session type replaces McSession_t; requires multicast capability. |
| `service_lora_rmvmulc(uint32_t devaddr)` | `rzi_lorawan_remove_multicast_session(uint32_t dev_addr)` | Mapped | Remove by DevAddr; requires multicast capability. |
| `service_lora_lstmulc(McSession_t *iterator)` | `rzi_lorawan_get_multicast_count(size_t *count)` `rzi_lorawan_get_multicast_session(size_t index, struct rzi_lorawan_multicast_session *session)` | Mapped | Count/index replaces iterator; enumeration omits keys. |
| `service_lora_clear_multicast(void)` | `rzi_lorawan_clear_multicast_sessions(void)` | Mapped | Clear sessions; requires multicast capability. |

</details>

<a id="rui3-map-3"></a>

## 4 service_lora_certification.h - 1 entries

<details>
<summary>service_lora_certification.h mapping (1 entries)</summary>

| RUI3 C declaration | RZI public API / destination | Mapping | Differences and limits |
| --- | --- | --- | --- |
| `service_lora_certification(int32_t mode)` | `rzi_lorawan_set_certification_mode(bool enabled)` `rzi_lorawan_get_certification_mode(bool *enabled)` | Partial | USP bool enable/query, not all RUI3 mode subcommands. |

</details>

<a id="rui3-map-4"></a>

## 5 service_lora_fuota.h - 0 entries

<details>
<summary>service_lora_fuota.h mapping (0 entries)</summary>

This header contains package/stack callbacks excluded from application mapping. RZI exposes rzi_fuota_init, register_callbacks, get_status, set_expected_size, read_image, and apply through [fuota.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/fuota.h). USP/LBM retain the packages.

</details>

<a id="rui3-map-5"></a>

## 6 service_lora_arssi.h - 1 entries

<details>
<summary>service_lora_arssi.h mapping (1 entries)</summary>

| RUI3 C declaration | RZI public API / destination | Mapping | Differences and limits |
| --- | --- | --- | --- |
| `service_lora_get_arssi(chan_rssi *iterator)` | — | Unmapped | No channel RSSI scan service/callback. Last-downlink RSSI is not equivalent. |

</details>

<a id="rui3-map-6"></a>

## 7 service_lora_test.h - 12 entries

<details>
<summary>service_lora_test.h mapping (12 entries)</summary>

| RUI3 C declaration | RZI public API / destination | Mapping | Differences and limits |
| --- | --- | --- | --- |
| `service_lora_trssi(int16_t *rssiVal)` | `rzi_lora_test_rssi(void)` `rzi_lora_test_get_rssi(int16_t *rssi_dbm)` | Mapped | Start/read split; getter returns BUSY until ready. |
| `service_lora_ttone(void)` | `rzi_lora_test_tone(void)` | Mapped | USP/LBM radio test mode. |
| `service_lora_toff(void)` | `rzi_lora_test_stop(void)` | Mapped | Stops radio tests, not service deinitialization. |
| `service_lora_trx(int32_t nb_packet)` | `rzi_lora_test_rx(uint32_t packet_count)` | Mapped | Receive-packet test through USP RAC/LBM. |
| `service_lora_ttx(int32_t nb_packet)` | `rzi_lora_test_tx(uint32_t packet_count)` | Mapped | Transmit-packet test through USP RAC/LBM. |
| `service_lora_set_tconf(testParameter_t *Param)` | `rzi_lora_set_config(const struct rzi_lora_config *config)` | Partial | Only rzi_lora_config fields are covered. No equivalent AFC, CRC-enable, fixed payload-size, hopping, or some test-timeout fields. |
| `service_lora_get_tconf(testParameter_t *Param)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Partial | Only rzi_lora_config fields are covered. No equivalent AFC, CRC-enable, fixed payload-size, hopping, or some test-timeout fields. |
| `service_lora_tth(const testParameter_t *param)` | — | Unmapped | RUI3 hopping test; single-frequency TX/RX is not equivalent. |
| `service_lora_get_cw(testCwParameter_t *param)` | `rzi_lora_get_config(struct rzi_lora_config *config)` | Partial | Generic frequency/power only; no full CW snapshot or duration getter. |
| `service_lora_set_cw(testCwParameter_t *param)` | `rzi_lora_test_cw(uint32_t frequency_hz, int8_t power_dbm, uint32_t duration_ms)` | Mapped | Hz/dBm; RUI3 txTimeout is seconds, RZI duration_ms is milliseconds. |
| `service_lora_set_dr_for_trth(SERVICE_LORA_DATA_RATE dr)` | — | Unmapped | No equivalent hopping-test control. Generic rate/power configuration does not provide the complete feature. |
| `service_lora_set_txp_for_trth(int8_t txp)` | — | Unmapped | No equivalent hopping-test control. Generic rate/power configuration does not provide the complete feature. |

</details>

<a id="rui3-map-boundaries"></a>

## 8 Structures and extensions

- Convert P2P fields individually into rzi_lora_config. Do not reuse old layouts or assume failed backend application rolls back RAM configuration.
- Multicast uses RZI structures/capability; enumeration omits keys. FUOTA exposes state, images, and apply, not package buffers/globals.
- Additional lifecycle/state/subscriber, GenAppKey, link-control, Storage-record, and Power APIs appear in the [public index](../api-design/public-index.md) without invented RUI3 origins.
- RUI3 suspend/resume acts on the radio; Power constraints/thread sleep act at other layers. Ping-slot channel and beacon-frequency setters have no equivalents.

<details>
<summary>Historical source snapshot: 183 application functions</summary>

Review date: 2026-11-09. SHA-256 values identify the original local RUI3 files. Of 202 unique declarations, 19 internals were excluded, leaving 183 rows. Counts are included functions; hashes cover complete files. Conditional unions do not imply simultaneous availability.

```text
service_lora.h (113)
7d5e2b012c0bbc25820b61b13f5949b4998662146b93a06e0b99f8b230fd2031

service_lora_p2p.h (52)
d3a13019d07b4e3a19a799ab99465ec2491d4e20d5eba1dc8fe8b98fb73ac329

service_lora_multicast.h (4)
af2d4141ebdd113c03bb2430afe014d540fe3d2910e17fb03b6afc0c19cf0fdc

service_lora_certification.h (1)
b873322ca90d0c2363367adad7e1a5c85da0be16a84f3de4f0a315759bc47491

service_lora_fuota.h (0)
4f93cf41033fbe0f04811752b4c658378bb0b56bf0783d2d87c354fdb09266f3

service_lora_arssi.h (1)
03c7d7cd3bdc1c285039145e0085bbe3ce87e33765f52b5dd98a7769c4ed905b

service_lora_test.h (12)
4adcd9e672275895fb55462dc62253f6f90a95d5b3b6731a446c94f9cb10c716
```

</details>

See also: [API design index](../api-design/index.md).
