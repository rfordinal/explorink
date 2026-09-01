# Crash reporting: what the device keeps, and how to read it

Two independent mechanisms record a crash. They have different coverage and
different failure modes. Know which one you are looking at.

| | `crash_report.txt` | ESP coredump |
|---|---|---|
| written by | our code, `lib/hal/HalSystem.cpp:96` | ESP-IDF, in the panic handler |
| lives on | SD card, `/crash_report.txt` | flash, partition `coredump` at `0xFF0000` |
| holds | version, panic reason, last log lines, stack words | registers, every task's stack, full backtrace |
| readable | open the card | needs the matching ELF and a gdb |
| survives | until the next crash overwrites it | until the next **panic** overwrites it |
| backtrace on Xtensa | **no, never** | yes |
| watchdog reset | **not written at all** | written |

Rule of thumb: `crash_report.txt` tells you *when* and *what was happening*.
Only the coredump tells you *where*.

## `crash_report.txt`

`HalSystem::checkPanic()` (`lib/hal/HalSystem.cpp:96`) runs early in
`setup()` (`src/main.cpp:375`), right after the SD card comes up. If the boot
was a panic boot it writes the file and the device later shows
`CrashActivity` (`src/main.cpp:513`).

The file holds `TRAILINK_VERSION`, the panic reason, the tail of the log ring
buffer, and a stack dump. The log tail is the useful part: it is the last thing
the firmware said before it died, and the first lines of the *new* boot are
appended to it, because the report is written after boot has started. The
timestamp resetting to `[1]` is where the crash was.

### Gap 1: the stack dump is always empty on Xtensa

`__wrap_panic_print_backtrace()` (`lib/hal/HalSystem.cpp:36`) only fills
`panicStack` under `#if !__riscv` being false — line 42 returns early on
anything that is not RISC-V. X4 is a C3 and gets a stack. **The T5 S3 Pro and
every other S3 board get nothing**, always, on every crash.

So on an S3, an empty `Stack memory:` block is not a fact about the crash. It
is the tool not working. Do not read anything into it.

### Gap 2: an empty panic reason means a CPU exception

`panicMessage` is only filled by our `__wrap_panic_abort()`
(`lib/hal/HalSystem.cpp:24`). In ESP-IDF 5.5.2, `panic_abort()` is reached only
from `esp_system_abort()`
(`components/esp_system/port/esp_system_chip.c:87`), which serves `abort()`,
`assert()`, ubsan and the stack-smash check.

A hardware CPU exception — LoadProhibited, StoreProhibited, IllegalInstruction,
InstFetchProhibited, alignment — does not go through it. So:

- **reason present** = an abort or an assertion
- **reason empty** = a CPU exception, and you need the coredump

### Gap 3: a watchdog reset writes no report at all

`isRebootFromPanic()` (`lib/hal/HalSystem.cpp:149`) accepts only
`ESP_RST_PANIC` and `ESP_RST_CPU_LOCKUP`. But ESP-IDF sets three different
hints (`components/esp_system/panic.c:450-458`):

- interrupt watchdog -> `ESP_RST_INT_WDT`
- task watchdog -> `ESP_RST_TASK_WDT` (also set at
  `components/esp_system/task_wdt/task_wdt.c:379`)
- everything else -> `ESP_RST_PANIC`

Both watchdogs are armed in `sdkconfig.defaults`: `CONFIG_ESP_INT_WDT_TIMEOUT_MS
= 300`, `CONFIG_ESP_TASK_WDT_PANIC = y` with a 5 s timeout. A five-second freeze
therefore resets the device, writes a **coredump**, and leaves **no SD report
and no crash screen**. It looks like a spontaneous reboot.

Tracked as T-234. Note also that the task watchdog sets `g_panic_abort = true`
directly instead of calling `panic_abort()`, so even if the reset reason were
accepted, the reason string would still be empty.

## Reading the coredump

Enabled on every environment: `partitions.csv:7` puts a 64 kB `coredump`
partition at `0xFF0000`, and `sdkconfig.defaults` sets
`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` with `..._DATA_FORMAT_ELF`.

**Archive the ELF that matches what is on the board before you do anything
else.** `docs/firmware-builds/` in the parent repo, next to the `.bin`. Without
the exact ELF the coredump is a list of numbers. Rebuilding "the same commit"
is not good enough if the build is not byte-identical.

Where the evidence ends up, and in what order: `docs/crashes/README.md` in the
parent repo. One directory per crash, and the two steps with a deadline —
copying the SD files and pulling the coredump — come before any analysis.

Verified recipe, 2026-09-01:

```
# 1. take the X4 lock, identify the port (303a is an Espressif board)
udevadm info -q property -n /dev/ttyACM0 | grep ID_VENDOR_ID

# 2. read the partition (read-only; it does reset the board)
esptool --chip esp32s3 -p /dev/ttyACM0 read-flash 0xFF0000 0x10000 coredump.bin

# 3. release the lock, then decode
~/.platformio/penv/bin/esp-coredump --chip esp32s3 info_corefile \
  --gdb ~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb \
  --core coredump.bin --core-format raw \
  --save-core core.elf \
  docs/firmware-builds/<the-matching>.elf
```

Two tooling notes:

- `esp-coredump` is not installed globally. It ships inside PlatformIO's venv at
  `~/.platformio/penv/bin/esp-coredump`.
- **gdb is not in the toolchain package.** `toolchain-xtensa-esp-elf` has gcc,
  objdump and readelf but no gdb, and `esp-coredump info_corefile` cannot work
  without one. Install it:
  `pio pkg install -g -t "platformio/tool-xtensa-esp-elf-gdb"`.

### Two traps in the output

**`.bss` is not captured.** `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` is off, so only
task stacks and TCBs are in the dump. gdb will happily print a global as all
zeros — those zeros come from the ELF's NOBITS section, not from the device. A
claim about a global's value at crash time needs a register or a stack slot
behind it, not a `p someGlobal`.

**gdb's unwind of the crashed frame can be wrong.** When `pc` is 0 the
top frames print as `?? ()` with mangled addresses. Read the raw registers
instead: on Xtensa `a0` is the return address with the window bits set — clear
the top nibble to `0x4...` and resolve it with
`info symbol` / `info line *0x...`.

### It gets overwritten

`CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` is **not** set, so the next panic
replaces the dump. After a crash worth understanding, extract before doing
anything that might crash again.

An ordinary `write-flash 0x10000 app.bin` does not touch `0xFF0000` and is
safe. `esptool erase-flash` destroys the dump.

## Worked example

[`ble-deinit-crash.md`](ble-deinit-crash.md) is the first crash solved this
way: SD report said nothing (empty reason, empty stack, both by design on S3),
coredump named the task, the instruction and the NULL pointer in one pass.
