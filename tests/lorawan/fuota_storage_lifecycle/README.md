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
