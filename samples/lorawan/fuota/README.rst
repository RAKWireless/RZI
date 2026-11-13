.. zephyr:code-sample:: rzi_lorawan_fuota
   :name: RZI LoRaWAN FUOTA
   :relevant-api: rzi_lorawan rzi_fuota

   Join a LoRaWAN network and accept ChirpStack FUOTA deployments through RZI.

Overview
********

This sample starts the stack, waits until it is ready, joins, then uses the Agent to schedule a
keepalive. Both sample modes use EU868, a 5-second interval and FPort 1.
The decoder accepts up to 200 fragments of up to 100 bytes each.
ChirpStack negotiates the actual count and size for each session.
``rzi_lorawan_init()`` initializes FUOTA coordination when
``CONFIG_RZI_LORAWAN_FUOTA`` is enabled. The application registers FUOTA
callbacks to observe session state and completion. RZI and the USP backend
handle clock sync, Class C multicast and image reconstruction.

Clock Synchronization (FPort 202), Remote Multicast Setup (FPort 200) and
Fragmentation (FPort 201) remain inside LoRa Basics Modem.

``application_key`` in the OTAA join config is the LoRaWAN 1.0.x
**GenAppKey**. Provision the same value on ChirpStack. If no separate
GenAppKey is configured, the SDK uses the OTAA AppKey.

Requirements
************

* An RZI RAK4631 (``rak4631/nrf52840/rzi``) or RAK3372-F (``rak3372_f``) with its LoRa antenna connected
* A LoRaWAN gateway and ChirpStack v4
* Device-profile application-layer packages enabled: Clock Sync v1,
  Remote Multicast Setup v1 and Fragmentation v1
* A short expected uplink interval in the device-profile, for example 5 s
* Valid OTAA identifiers and matching device and server keys

See ``doc/fuota.md`` for the ChirpStack deployment checklist and image-size
limits of the default LBM context partition.

Building
********

From the workspace root:

.. code-block:: console

   west build -p always -b rak4631/nrf52840/rzi --sysbuild RZI/samples/lorawan/fuota

For RAK3372-F use ``-b rak3372_f/stm32wle5xx``. Console is LPUART1.
Flash ``build/merged_rak3372_f_stm32wle5xx.hex``.

Credentials are in ``app.overlay``, with the same OTAA properties as
``samples/lorawan/periodical_uplink`` and an optional separate GenAppKey.
Flash ``build/merged_rak4631_nrf52840_rzi.hex``. See ``doc/boot.md``.

FUOTA needs the secondary image slot. This sample builds for
``rak4631/nrf52840/rzi`` and ``rak3372_f`` (slot1 on the MX25L400).
``rak3172/stm32wle5xx/rzi`` is single-slot.

Candidate ownership
*******************

The default build retains the completed candidate in backend storage.
``DATA_READY`` makes its contents available for bounded reads. After
``FINISHED``, the application can validate the image and explicitly call
``rzi_fuota_apply()``, or discard it with ``rzi_fuota_release_data(generation)``.
A retained candidate keeps its storage reserved for that generation.

To exercise a receiver that consumes an opaque block, build the same sample:

.. code-block:: console

   west build --sysbuild -b rak3372_f/stm32wle5xx -d build/fuota-demo RZI/samples/lorawan/fuota -- -Dfuota_CONFIG_APP_FUOTA_RECEIVE_DEMO=y

In receive-demo mode, ``FINISHED`` wakes the application loop. It checks the
generation, reads the entire block in chunks, then releases that generation so
the next deployment can use the storage. A read error leaves the candidate
available for inspection. Neither mode requests an automatic install.
Both use the same join, callback registration, keepalive and FPort 101 reporting.
Use local build overrides for lab credentials, region or test limits.

See the `receive agent design <../../../doc/fuota-agent.md>`_ for the Class A
and Class C keepalive timing. The `Agent test guide
<../../../tests/lorawan/fuota_agent/README.md>`_ covers the independent host
suite and individual callback regression cases.

Expected output
***************

On the console UART at 115200 baud, with timestamps omitted:

.. code-block:: console

   <inf> rzi_fuota_sample: RZI LoRaWAN FUOTA sample
   <inf> rzi_fuota_sample: Joining
   <inf> rzi_fuota_sample: Joined
   <inf> rzi_fuota_sample: TXDONE_SENT

When ChirpStack starts a deployment, the multicast and fragmentation setup
arrives as downlinks, then:

.. code-block:: console

   <inf> rzi_fuota_sample: FUOTA transferring
   <inf> rzi_fuota_sample: FUOTA transfer started
   <inf> rzi_fuota_sample: FUOTA transfer complete

Receive-demo mode also logs ``Read and released FUOTA generation <n>`` after
a successful read and release, or ``FUOTA consumer failed for generation <n>``
on error.

The state line uses the ``enum rzi_fuota_state`` name: ``idle``, ``ready``,
``transferring``, ``complete``, ``failed`` or ``applying``.

FPort 101 reception events
*************************

The managed receiver snapshots each session before releasing its decoder.
At multicast start it queues event 0x01 with the negotiated fragment count
(uint16 little-endian), size (one byte) and session index (one byte). Log
transmission resumes after the original radio settings have been restored.

If an entered multicast session times out incomplete, it queues one 0x08
after finalization. Event Data holds the distinct received count and the
negotiated count, both uint16 little-endian. Retransmissions keep their
original number; a distinct redundancy fragment contributes one. The TS004
status answer keeps its own counters and scheduling.

A checked fragment write failure reports its original errno in the UART log.
When all four target-version bytes (business header offsets 12 through 15)
are available it queues FLASH_FAIL (0x07). An unavailable version leaves
the cause in the local log. Backends offering only legacy lifecycle callbacks
have no session statistics to encode these FPort 101 records.

Troubleshooting
***************

Joined, but no ``FUOTA transfer started``
   The device-profile does not enable all three application-layer packages,
   or the deployment has not reached the multicast session start time yet.

``FUOTA transfer failed: RZI_ERR_*``
   The name comes from ``include/rzi/err.h``. Compare it with the deployment
   status in ChirpStack, and check the image-size limits in ``doc/fuota.md``.

Join problems
   See the troubleshooting section of ``samples/lorawan/periodical_uplink``.

Test payload
************

``test_payload/rzi-fuota-test.bin`` is a 4096-byte ChirpStack upload file.
It starts with the ASCII magic ``RZI1`` (hex ``52 5a 49 31``). Use receive-demo
mode to exercise reading and releasing this payload.

Regenerate it with:

.. code-block:: console

   python3 samples/lorawan/fuota/test_payload/gen_test_bin.py
