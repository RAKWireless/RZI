# 3. Call paths and runtime relationships

Status: normative

<a id="overview"></a>

The text trees trace major USP calls and continuation steps. Checks/secondary branches remain in source. Asynchronous events are separate from synchronous requests and do not imply blocking waits.

<a id="dispatch"></a>

<details>
<summary>Source layout</summary>

```text
include/rzi/lorawan/*.h
         ↓
lorawan.c                 Lifecycle, join/leave/send, state, subscribers, dispatcher
api/*.c                   Configuration, credentials, channels, domain facades
         ↓
backend/lorawan_backend.h  base + typed optional ops
         ↓
backend/usp/*.c → USP / LoRa Basics Modem
         ↓
stack -> backend event -> fixed queue -> dispatcher -> callbacks / hooks
```

Runtime controls usually check arguments/readiness before ops dispatch. Credentials/settings may use caches/Storage before init. Not every facade follows one ready -> ops path.

</details>

<a id="call-init"></a>

## 3.1 Shared infrastructure and initialization

USP init runs in the caller thread. RESET may arrive asynchronously or replay synchronously on handler registration; public delivery still uses the event sink.

<details>
<summary>Initialization call tree</summary>

```text
rzi_init()                                      [src/common/common_init.c]
└─ rzi_storage_init()                           [CONFIG_RZI_STORAGE only]
   └─ settings_subsys_init()

rzi_lorawan_init(region)                         [src/lorawan/lorawan.c]
├─ check_backend_contract()
├─ rzi_lorawan_config_set_region(region)
├─ rzi_lorawan_backend.base.init → usp_init()     [backend/usp/lorawan_backend_usp.c]
│  ├─ rzi_usp_acquire()                          [first backend reference]
│  │  └─ rzi_usp_init()                          [src/usp/usp_platform.c]
│  │     ├─ SMTC_SW_PLATFORM_INIT()              [first platform init only]
│  │     ├─ smtc_rac_init()
│  │     └─ smtc_modem_init(modem_event_callback)
│  └─ rzi_usp_set_event_handler(handle_event)
└─ attached[i]->on_initialized()                 [registered private hooks]

[modem RESET; handler registration may replay a recorded RESET]
handle_event()
├─ configure_service()
│  ├─ check_region_built()
│  ├─ smtc_modem_set_region()
│  └─ smtc_modem_set_join_duty_cycle_backoff_bypass()
└─ rzi_lorawan_usp_publish(READY) -> event delivery in Section 3.3
```

</details>

Shared init does not start the modem. LoRaWAN init success does not mean joined; normal join follows READY. RESET replay may run in the registration thread. Configuration failure publishes ERROR, not READY.

Sources: [shared init](https://github.com/RAKWireless/RZI/blob/main/src/common/common_init.c), [core](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c), [USP init/RESET](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c), [platform](https://github.com/RAKWireless/RZI/blob/main/src/usp/usp_platform.c).

<a id="call-join"></a>

## 3.2 Join: policy, USP, and LBM

The service calls USP/LBM. The USP thread queues backend events; the service dispatcher consumes them and invokes subscribers.

<details>
<summary>Join call tree</summary>

```text
rzi_lorawan_join(start, auto_join, interval, attempts)
├─ rzi_lorawan_credentials_update_policy()       [save policy first]
├─ stop_join()                                  [start = 0]
└─ begin_join()                                 [start = 1, state permits]
   ├─ rzi_lorawan_get_activation()
   ├─ rzi_lorawan_get_join_attempts()
   ├─ rzi_lorawan_credentials_build_config()
   └─ rzi_lorawan_backend.base.join → usp_join()
      ├─ [OTAA]
      │  ├─ configure_credentials()
      │  │  ├─ smtc_modem_set_deveui()
      │  │  ├─ smtc_modem_set_joineui()
      │  │  ├─ smtc_modem_set_appkey()
      │  │  └─ smtc_modem_set_nwkkey()
      │  ├─ configure_channel_mask() → smtc_modem_set_channel_mask()
      │  ├─ lorawan_api_set_activation_mode(LBM_ACTIVATION_OTAA, ...)
      │  └─ smtc_modem_join_network()
      └─ [ABP]
         ├─ configure_channel_mask()
         └─ smtc_modem_debug_connect_with_abp()

[asynchronous JOINED / JOINFAIL]
modem_event_callback() → handle_event() → rzi_lorawan_usp_publish()
→ publish() → queue_event() → dispatcher() → dispatch()
  ├─ swallow_join_event()                       [count retries, suppress intermediate failures]
  ├─ update_state_from_backend()                 [events not suppressed]
  └─ callbacks->on_event(..., user_data)         [JOINED / final JOIN_FAILED]
```

</details>

This is the main start=1 success path. Stop, policy-only, and failure branches are omitted. Intermediate failures may be suppressed. Save/start are not transactional. KEEP updates policy only; ABP transmits no Join-Request.

Sources: [join/retries](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c), [policy](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/api/lorawan_credentials.c), [USP](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c).

<a id="call-send"></a>

## 3.3 Uplink and downlink paths

TXDONE is converted/published directly. DOWNDATA first reads LBM downlinks through publish_downlinks. Cross-thread delivery follows queuing.

<details>
<summary>Send and event call tree</summary>

```text
rzi_lorawan_send(port, data, size, type)
├─ check_joined()
│  ├─ check_context()
│  └─ rzi_lorawan_backend.base.is_joined → usp_is_joined()
├─ rzi_lorawan_mac_commands_on_uplink()
└─ rzi_lorawan_backend.base.send → usp_send()
   ├─ rzi_lorawan_usp_enter()
   ├─ smtc_modem_request_uplink()
   └─ rzi_lorawan_usp_finish()

[asynchronous modem event, not a synchronous child of send]
modem_event_callback()                           [src/usp/usp_platform.c]
├─ smtc_modem_get_event()
└─ handler(...) → handle_event()                 [USP LoRaWAN handler]
   ├─ [TXDONE] convert transmit status
   └─ [DOWNDATA] publish_downlinks()
      └─ smtc_modem_get_downlink_data()
   Both branches publish through rzi_lorawan_usp_publish()
   └─ event_sink(...) -> publish()                [sink bound in usp_init]
      └─ queue_event() → k_msgq_put(..., K_NO_WAIT)

[RZI dispatcher thread]
dispatcher()
├─ k_msgq_get()
└─ dispatch()
   ├─ update_state_from_backend()
   ├─ event_from_backend()
   ├─ callbacks->on_event(..., user_data)         [TXDONE / DOWNDATA]
   └─ attached[i]->on_event(...)                 [private optional services]
```

</details>

Submission sets tx_pending and releases the USP lock. Further sends return BUSY. TXDONE clears the flag before queuing. RESET also clears it, so a cleared flag does not establish the original outcome.

Application callbacks run in the dispatcher, not inside modem submission. Chapter 5 covers loss.

Sources: [send/dispatcher](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c), [USP traffic](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c), [modem events](https://github.com/RAKWireless/RZI/blob/main/src/usp/usp_platform.c).

DOWNDATA is independent reception, including Class B/C windows. TXDONE and DOWNDATA are separate handle_event branches publishing to the same queue.

<a id="call-ops"></a>

## 3.4 Domain dispatch: join-backoff query

CALL_NETWORK_OP expands into readiness/member checks, not another runtime layer. USP queries share one enter/finish pair; errors skip later queries and release the lock.

<details>
<summary>Join-backoff query call tree</summary>

```text
rzi_lorawan_get_join_backoff(&remaining_s)         [api/lorawan_network.c]
└─ CALL_NETWORK_OP(get_join_backoff, remaining_s) [macro expansion]
   ├─ rzi_lorawan_check_ready()
   ├─ Check network table and get_join_backoff member for NULL
   └─ rzi_lorawan_backend.network->get_join_backoff → usp_get_join_backoff()
      ├─ rzi_lorawan_usp_enter()
      ├─ smtc_modem_get_status()
      ├─ smtc_modem_get_join_duty_cycle_backoff_bypass()
      ├─ smtc_modem_get_certification_mode()
      ├─ lorawan_api_next_join_time_second_get()  [not joined, no bypass/certification]
      ├─ smtc_modem_hal_get_time_in_s()          [same branch; remaining seconds]
      └─ rzi_lorawan_usp_finish()
```

</details>

Sources: [facade/macros](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/api/lorawan_network.c), [USP network](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp_network.c).

Certification mode is also queried. Remaining time is computed only when not joined, without bypass/certification. Other successful branches output zero; errors return early.

See also: [API design index](index.md).
