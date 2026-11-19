# Power management

Status: implemented

RZI uses Zephyr's system PM states, policy, and reference-counted locks.
There is no RZI sleep-mode enum or second state-selection policy.
See [power-api.md](../api/power.md) for the C API.

## RZI API boundary

Keep `rzi_power_*` as the application-facing API and translate its operations
to Zephyr in `power.c`. This boundary leaves room for future implementation
changes without introducing another power-management model.

- Reuse native state meanings and lock/event semantics. RZI-prefixed state
  names, when needed by an API, must map one-to-one to native states.
- Let Zephyr select states and perform transitions. Keep platform wake and
  peripheral configuration in the board and drivers.
- Retain only the translation and reference ownership required by existing
  operations. Extend the interface when a concrete caller needs it.

## Native states and ownership

Zephyr defines `enum pm_state` in `<zephyr/pm/state.h>`:

| Native state | Meaning |
|---|---|
| `PM_STATE_ACTIVE` | Active execution |
| `PM_STATE_RUNTIME_IDLE` | Runtime idle; devices retain their existing states |
| `PM_STATE_SUSPEND_TO_IDLE` | Suspend without losing CPU execution context |
| `PM_STATE_STANDBY` | Platform standby |
| `PM_STATE_SUSPEND_TO_RAM` | Suspend with context retained in RAM |
| `PM_STATE_SUSPEND_TO_DISK` | Save context to nonvolatile storage before suspend |
| `PM_STATE_SOFT_OFF` | Power off without preserving execution context |

These names describe Zephyr states. Their availability and hardware mapping
come from each SoC's device tree and PM driver. They are not interchangeable
across boards. For example, STM32WL maps STOP0/1/2 to
`PM_STATE_SUSPEND_TO_IDLE` with different substate IDs.

The application enables `CONFIG_PM`. The idle thread and Zephyr PM policy
then select a state using configured `cpu-power-states`, residency times,
exit latency, pending events, and native locks. The power service does not
select a specific state, force STOP2, or promise RAM retention for arbitrary
boards. On STM32WL, `AT+LPMLVL` locks the unused STOP substate from the AT
command.

| Layer | Responsibility |
|---|---|
| Application / board | `CONFIG_PM`, device tree, wake peripherals, native state constraints |
| Zephyr | State selection, idle entry/resume, device PM |
| RZI power | Runtime PM-lock switch, named service references |
| LoRaWAN backend | TX/RX1/RX2 timing, radio IRQs, stack timers |

`CONFIG_RZI_POWER` enables the helpers without selecting `CONFIG_PM`.
Ordinary CPU idle still works when system PM is disabled. Device runtime PM
is independent of the system PM switch.

## P2P activity and low-power policy

Status: implemented for the USP P2P backend; hardware acceptance pending.

For P2P LoRa and FSK, an enabled receive operation or an in-flight transmit
must prevent system STOP entry. When neither operation is active, the system
may enter low power, subject to the LPM switch, other service locks, and
Zephyr policy. Selecting P2P mode or initializing its service alone must not
keep the system awake. Ordinary kernel CPU idle is distinct from STOP entry.

| P2P activity | Required behavior |
|---|---|
| No RX and no TX | Permit low power; park the radio when no other radio owner needs it. |
| Timed or continuous RX enabled | Hold an activity PM reference for the entire receive operation. |
| TX in progress | Hold an activity PM reference until completion, timeout, or cancellation. |
| RX ends or is explicitly stopped | Release the RX reference once reception has actually stopped. |
| TX finishes while continuous RX remains enabled | Release only TX ownership; keep RX protected. |

Use paired operation ownership, including submission failures and abort paths.
Deliver pending completions before discarding their processing responsibility.
An RF completion interrupt is not the same as delivery of an application
callback. Preventing STOP does not unblock a thread waiting forever on a
semaphore. With the USP backend, P2P still needs `rzi_lora_poll()` to dispatch
queued results; the USP backend wakes the AT thread via `rzi_at_wake()` as soon
as a TX or RX completion is queued, avoiding periodic timer wakeups during idle
or continuous RX.
Startup extension processing runs before the first UART wait.
A separate always-running P2P dispatcher is not required by this policy.

Reference behavior was reviewed in the local RUI3 source
`component/service/lora/service_lora_p2p.c`: `service_lora_p2p_send()` takes a
wake lock; `service_lora_p2p_recv()` takes one for enabled reception and
unlocks on explicit stop; completion/timeout handlers release ownership
when the operation ends. RUI3 continuous modes retain reception as appropriate.
This is a behavioral reference, not a requirement to copy its lock accounting
or its special receive-mode numeric values into RZI.

The USP backend acquires one native all-state PM reference before submitting
RX/TX and retains it while either operation or deferred completions remain.
It releases the reference from poll after the work is drained, or on successful
stop/failed submission when no work remains. This protection depends on `CONFIG_PM`,
not `CONFIG_RZI_POWER`; it is separate from the named RZI power-lock mask.
Native clients must continue to call `rzi_lora_poll()` until completions are
drained, otherwise the reference deliberately remains held. The USP receive
activity query now reads backend state, including timeout and one-shot endings.
The blocking Zephyr backend is not changed by this implementation.

### STOP while receiving (`CONFIG_RZI_LORA_USP_RX_IRQ_WAKE`)

When `CONFIG_RZI_LORA_USP_RX_IRQ_WAKE=y` (default on STM32WL SUBGHZ when
`CONFIG_PM=y`), an open P2P receive window does not hold the PM reference
while listening. On STM32WL the on-chip
SUBGHZ radio keeps its own HSE/TCXO oscillator active in RX while the CPU
enters `STOP1` or `STOP2`, and EXTI line 44 wakes the MCU on `RxDone` or
`RxTimeout`:

| State | PM reference |
|---|---|
| RX listening, queue empty | Not held (`STOP` allowed) |
| TX in flight | Held until TX completion, timeout, or cancellation |
| Packet queued, not yet delivered | Held until `rzi_lora_poll()` drains it |
| RX cancel pending | Held until the RAC abort callback completes |
| Packet delivered or RX stopped | Released |

Continuous RX (`65535`) chains 60 s RAC windows, so the MCU wakes once per
minute to resubmit the window in addition to waking immediately on received
packets. Boards whose radio IRQ cannot wake from `STOP` keep this option off.

## Runtime PM switch

`rzi_power_set_pm_enabled(false)` takes one native
`pm_policy_state_all_lock_get()` reference. Passing `true` releases only
that reference. Repeated calls are idempotent; enabling does not release
PM locks or locks owned by other code.

`rzi_power_get_pm_enabled()` reads this same switch for both API and AT
clients. It returns false if `CONFIG_PM` is disabled. Changing the switch
without system PM returns `-RZI_ERR_NOT_SUPPORTED`.

`AT+LPM=0` acquires the switch's lock; `AT+LPM=1` releases it. `AT+LPM=?`
reads the switch. A value of 1 means this switch permits system PM, not
that a specific state is reachable or that every native lock is released.
A system PM lock does not prevent ordinary CPU idle or control device
runtime PM.

Applications that need state-specific restrictions use Zephyr directly:

```c
#include <zephyr/pm/policy.h>

/* Prevent this native state and all its substates during an operation. */
pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
/* Perform the operation. */
pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
```

The native lock API only affects states present in the device tree; it
neither adds a state nor enables an unsupported hardware mode.

## Service PM locks

Each successful `rzi_power_lock_get()` takes one native all-state PM lock.
Each matching `rzi_power_lock_put()` releases one. RZI tracks counts by
owner so a mismatched release fails instead of unlocking somebody else's
reference.
Storage aborts before a write/delete when its reference cannot be acquired.
Class C state and reference ownership are updated under the same config
mutex, including releases on leave.
Without `CONFIG_PM`, only reference counts are tracked; these references
do not restrict ordinary CPU idle or device runtime PM.

With `CONFIG_RZI_POWER=y`:

| PM lock | Owner |
|---|---|
| `LORAWAN` | Class C; released when leaving Class C or leaving the network |
| `FLASH` | Settings writes/deletes and FUOTA transfer/apply |
| `APP`, `TRANSPORT`, `UPDATE` | Application-managed, paired references |

Class A join/send does not hold a PM lock for the entire exchange. The
backend schedules TX and receive windows so the CPU may sleep between
radio events. RZI does not calculate or delay RX1/RX2.

## Thread waits

`rzi_power_sleep(int64_t timeout_ms)` is a thin `k_sleep()` wrapper. A
negative value waits forever, and `k_wakeup()` may end the wait early.
Native timeout-range limits apply, without an additional 32-bit millisecond
cap. System sleep follows when the scheduler has no runnable work and the
native PM policy permits it.

Use a kernel timer or delayed work to run an action at a given time.
Periodic uplinks already scheduled by delayed work need no additional
application sleep or RZI PM event.

## Current periodic-uplink boards

- RAK3172 and RAK3172-F enable `CONFIG_PM` in their sample board configs.
  The sample uses SysTick with an RTC companion; Zephyr selects among the
  STM32WL STOP substates when possible.
- The current nRF52840 and RAK3362/nRF54L15 configurations leave system PM
  disabled. Their ordinary CPU idle path continues to work; the runtime
  system PM switch cannot disable that path. RAK3362 separately enables
  device runtime PM for its configured peripherals.

A new ESP or nRF target uses its native PM driver and device tree. It does
not need a new RZI mode or a platform-name switch in the state-selection
code. Physical current and radio timing still require board testing.

All persistent coordinator state is statically initialized; the power APIs
require no explicit initialization.

The implementation is in `src/power/power.c`; API and PM-lock tests are in
`tests/power/api`. AT integration tests are in `tests/at/framework`.
