# FUOTA storage lifecycle regression

Run in an initialized RZI west workspace:

```sh
west twister -T RZI/tests/lorawan/fuota_storage_lifecycle -p native_sim/native --inline-logs
```

The suite runs the real FUOTA Service and LoRaWAN dispatcher with native Flash.
The radio backend, USP hooks and control-answer occupancy are test doubles.
`CONFIG_IMG_MANAGER` is disabled: no bootloader installation or hardware is used.

The COMPLETE callback is deliberately held open while another caller attempts a
new Setup. Setup must return 0x02, preserve generation and erase count, and retain
all 4096 candidate bytes. This controlled pause is a race injection, not an
example of how an application callback should behave.

Additional cases cover committed-candidate Delete protection, stale generations,
delta readback, pending-answer protection, decoder failure without a Flash fault,
the first mapped storage error surviving a later write/decoder failure, timeout,
and bounds failure without a fabricated storage errno. Callback thread identity
and prompt StatusAns-driven finishing are outside this suite.

## Installation entry gates

The `rzi.lorawan.fuota_apply_gates` variant enables the Service's image-manager
branch with instrumented Boot and reboot functions. It adds public-interface
cases for early reconstruction, the pending-answer finish gate, candidate
retention after a failed TEST request, and an application request concurrent
with automatic installation. A pending or concurrent request must return
`-RZI_ERR_BUSY` without starting another Boot operation. The automatic path
must still proceed and preserve the candidate if TEST fails.

The normal variant keeps IMG_MANAGER disabled. Both variants use the actual
Service, dispatcher and native Flash, with simulated radio/USP hooks. The
installer variant does not execute MCUboot, alter a physical trailer, reboot
a device or establish boot/confirm/revert acceptance. Its first Boot call is
deliberately held with a semaphore to make the concurrent request repeatable.

Run both variants with the existing pinned Zephyr host environment:

```sh
west twister -T RZI/tests/lorawan/fuota_storage_lifecycle --integration --inline-logs
```
