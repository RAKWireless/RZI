# FUOTA agent host tests

[Simplified Chinese](README_cn.md)

Run this suite after changing the receive state machine, service callbacks or
keepalive scheduling. Zephyr `native_sim` runs the RZI service and agent with
a test backend that injects setup, reception, timeout and control TX results.

## Run the suite

Use a Linux or WSL west workspace with the project dependencies, the Twister
Python requirements, CMake, Ninja and GCC with 32-bit native_sim support.
From the RZI checkout:

```sh
cd "$(west topdir)"
export ZEPHYR_BASE="$PWD/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=host
out=build/fuota-agent-readme
python3 zephyr/scripts/twister -T RZI/tests/lorawan/fuota_agent -p native_sim -O "$out"
```

Check how many test cases **executed** and passed. Per-case results are in
`$out/twister.json`. For an assertion failure, find the case name and the first
failed assertion in `handler.log`. For a build failure, inspect `build.log`.

```sh
find "$out" \( -name handler.log -o -name build.log \) -print
```

For the local integration worktree, the following WSL command opens the Zephyr
build container at the workspace root. Both checkouts are mounted because
they share Git objects. Continue from the exports above inside the container.

```sh
cd /home/ian/workspace/rzi_fuota_task11
baseline=/home/ian/workspace/rzi_fuota
docker run --rm -it --entrypoint bash --user "$(id -u):$(id -g)" \
  -e HOME=/tmp -v "$PWD:/workdir" -v "$baseline:$baseline:ro" -w /workdir \
  ghcr.io/zephyrproject-rtos/zephyr-build:latest
```

## Run one case

After a suite build, use the simulator from the same output directory.
Rebuild after editing the sources.

```sh
sim=$(find "$out" -type f -name zephyr.exe -print -quit)
"$sim" -test=rzi_fuota_agent::test_class_c_no_query_resumes_one_period_after_hold
```

This case receives all fragments early on an original Class C device, then
receives no status query. The first keepalive must wait until the window end,
the status hold and one full keepalive period have passed. Check that the
selected case reports PASS and the summary shows one pass and zero failures.
The remaining cases are skipped. To run another case, use the second argument
of its `ZTEST` declaration in [src/main.c](src/main.c) after `::`.

| Behavior | Case |
| --- | --- |
| Restore the original A/C class and restart periodic traffic | `test_success_restores_then_resumes_periodic_by_class` |
| Process status and delete after timeout before the first keepalive | `test_timeout_status_then_delete_before_first_keepalive` |
| Accept a new generation after deleting fragment-only preconfiguration | `test_delete_fragment_only_preconfig_allows_new_generation` |
| Reject completion after losing events | `test_lost_events_never_report_complete` |

## Callback regression baseline

Several cases register a listener through the public FUOTA API and compare each
callback with the expected sequence, including its listener and thread. Search
`handler.log` for `CB_BASELINE` (scenario, ordinal, notification, listener,
thread, result) and `CB_TOTAL` (observed count). A missing, extra or reordered
notification fails the case. The count covers the interval starting before
PRECONFIG and ending at a synthetic TIME event sent after the scenario. The
TIME event drains earlier backend events and is excluded from the count.

`control.status+delete` means both bits are set in the snapshot at that callback.
The receive-timeout test for Class C injects two successful status completions;
the separate status-then-delete test injects one of each. To exercise callback
replacement during CLOSED, run `test_early_complete_retention_and_repeat`.
That case also probes apply with IMG_MANAGER disabled and explicitly releases
data on the test thread.

## Timing and scope

[prj.conf](prj.conf) sets the status hold to three seconds and the keepalive
period to one second. These tests check state changes, callback order and
backend calls. For the radio TX time and the ChirpStack ten-second response
window, correlate device, LNS and gateway logs. See the
[receive agent design](../../../doc/fuota-agent.md#examples-and-tests).
