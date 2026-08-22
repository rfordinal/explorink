# Panel goes pale on a long hot ride

Field report, 2026-08-22, X4 on a motorbike, sunny day, engine running:
**large parts of the panel came back pale after a refresh** -- big white patches,
weak contrast, at times nearly unreadable. It recovered as the marker moved and
more refreshes landed. The maintainer named three suspects: direct sun, heat,
and heavy vibration from the bike.

This doc says what the code actually does that can produce that, ranks the
suspects, and states what would settle each. **Nothing here is measured** -- the
device was lost on the same ride (`docs/mounting.md` in the parent repo), so
every hardware check below is blocked until a replacement arrives.

## The one real bug the code shows: the map never cleans the panel

`MapActivity` runs `HALF_REFRESH` **only on the entry frame**
(`src/activities/map/MapActivity.cpp:4511`, `:4998`, `:5414` -- all gated on
`pendingEntryCleanRefresh_`). Every frame after that is `FAST_REFRESH`
(`MapActivity.cpp:4553`, `:4752`), which on the X4 selects the controller's
partial/DU waveform (`0x22 = 0xFC`, `Ssd1677Driver.cpp:53-65`,
`docs/refresh-modes.md`).

DU is a shallow waveform. It drives only the pixels that differ from the RED
plane, with short pulses and no clearing inversion. Every e-ink vendor pairs it
with a periodic full clean, and **this SDK already does that on two other
controllers**:

- IT8951: `ghostClearInterval = 8` -- promote a differential refresh to a GC16
  clean every 8 partials (`It8951Driver.cpp:66`, `:365-368`).
- Murphy UC8253: same idea, `ghostClearInterval` (`Uc8253MurphyDriver.cpp:154-165`,
  `Uc8253MurphyDriver.h:17-18`).

`Ssd1677Driver` has **no such counter**. Grep it: the only promotions are
`_needsInitialFull` after boot/wake, cold panel, and leaving grayscale
(`Ssd1677Driver.cpp:355-394`; the list is in `docs/refresh-modes.md`, "What the
driver promotes behind your back"). So on the map screen the X4 can run **hours
of DU refreshes with zero cleans** -- a ride is one entry frame plus one DU per
marker move or repaint, forever.

That matches the report on both counts: the picture degrades gradually over a
long session, and further marker moves *improve* it, because each DU pass
re-drives the pixels it touches and deepens ink that the previous pass left
half-driven.

**Confidence: read off the source, high.** The mechanism is standard e-ink DU
behaviour; the missing counter is a fact in the tree.

## Heat: real, and the code makes it worse in one place

Two things stack on top of the missing clean.

**1. The panel gets hot and the waveform gets weaker.** The X4 sits in the sun
on a bike with no airflow shading it. `FAST` (`0xFC`) and `FULL` (`0xF7`) both
set the "load temperature" bit, so the controller reads its internal sensor and
picks the OTP waveform for that temperature bin (bit map corroborated by the
driver's own power-down handling, `Ssd1677Driver.cpp:288` -- `seqOverride & 0x03`
is the disable-analog/clock pair). Init puts the controller on its internal
sensor (`0x18 = 0x80`, `Ssd1677Driver.cpp:159-165`), so that bin follows the real
panel temperature. Hot bins use shorter pulses. Shorter pulses
plus a shallow DU waveform plus no clean is exactly a pale, mottled panel. Above
the top of the OTP table the waveform cannot get any shorter and the drive is
simply insufficient.

**Open:** the GDEQ0426T82 operating range. It is not in this tree, and a vendor
sheet is needed -- do not quote a number from memory. E-ink panels of this class
are typically specced to a ceiling well below what a black case in direct sun
reaches, but that sentence is a guess until the sheet says so.

**2. `HALF` lies to the panel about the temperature.** The `HALF` path writes a
fixed value to the temperature register before every clean:

```
bus.cmd(CMD_WRITE_TEMP);          // 0x1A
bus.data(_cfg.halfRefreshTemp);   // 0x5A  -- Ssd1677Driver.cpp:274-275, :309-310
```

and `0xD7` does **not** have the load-temperature bit set, so the controller
never overwrites that value from its sensor. The default X4 config carries
`0x5A` with the comment "HALF refresh temperature"
(`Ssd1677Driver.cpp:57`), copied from stock X4 firmware (`HALF: 3C=C0, 1A=5A,
22=D7, 20`, `Ssd1677Driver.cpp:45-51`). That is the "warmed" in
`docs/refresh-modes.md`'s table: telling the panel it is hot buys the shorter,
single-pass clean instead of the multi-flash one.

So **the X4's only clean primitive is deliberately under-driven**, at every
temperature, by design. It is fine at room temperature on a reader. On a hot
panel it is the wrong direction to be wrong in.

**Open:** the unit `0x5A` is in. One byte where the SSD16xx family documents a
12-bit register, so it is the high byte -- whether that lands on 90 C or
something else needs the SSD1677 datasheet. Stock firmware writes the same
byte, so the value is at least vendor-sanctioned.

## Vibration: unlikely, and here is why

Ranked last, and not because a bike does not shake. Microcapsule ink is
suspended in a solid binder; ordinary vibration does not move pigment. The two
paths that could matter are both mechanical-electrical, not optical:

- **FPC/connector disturbance.** A marginal panel flex under vibration corrupts
  SPI writes. That looks like torn lines, shifted blocks or garbage -- not soft,
  even paleness. Not what was seen.
- **Boost-rail sag.** The panel's charge pump needs its high gate/source rails
  to drive ink. Brownout under load would weaken the whole frame. Plausible in
  principle, but the X4 exposes no way to measure it, and nothing may be wired
  into the charge path or the cell (parent `CLAUDE.md`, "Never open a device").

So: vibration stays a suspect only for the connector case, and the observed
symptom argues against it.

## What the firmware can measure about this today: almost nothing

- **No panel temperature readback.** The X4's EPD bus is write-only -- its pin
  set is `{sclk 8, mosi 10, cs 21, dc 4, rst 5, busy 6}` with no MISO
  (`BoardConfig.h:691`), so the controller's own sensor cannot be read.
- **No board temperature sensor.** The SHT40 belongs to the Seeed Sticky profile
  (`BoardConfig.h:1040-1041`), not the X4.
- **The ESP32-C3 die sensor is the only reading available**, and nothing in the
  tree uses it (no `temperature_sensor` hit in `src/` or `freeink-sdk/`). Die
  temperature is a poor proxy for glass temperature, but it is a real number, it
  is free, and it is enough to tell a 20 C ride from a 50 C one.

## What to do (proposed, none of it written)

1. **Add `ghostClearInterval` to `Ssd1677Config`**, same shape as the IT8951 and
   Murphy configs, and promote a whole-panel `FAST` to `HALF` every N. Cost is
   known: 1,684 ms instead of 500 ms on that frame (`docs/refresh-modes.md`,
   "What it costs"). N wants to be a ride-tolerable number, not 8 -- a clean
   every 8 marker moves is a lot of 1.7 s freezes.
2. **Count time, not only refreshes.** A rider standing still gets no marker
   moves and so no cleans under a pure counter.
3. **Stop spoofing `0x5A` when the panel is hot** -- or make `halfRefreshTemp` a
   value the firmware picks, so a hot panel gets the honest temperature (longer
   waveform, better drive) and a cold one keeps the fast clean.
4. **A rider-visible "clean panel" action** in the map menu. Whatever the
   automatic rule turns out to be, a pale panel mid-ride needs a one-press fix
   that does not require leaving and re-entering the map.
5. **Log the C3 die temperature** into the refresh log, so the next ride
   produces evidence instead of another impression.

Order matters: 1 and 4 are worth having whatever the temperature story turns out
to be, because the missing clean is a bug on its own at any temperature.

## What would settle it

Every item needs a device. Blocked.

- **Missing clean vs heat.** Ride (or bench-run) the map for an hour at room
  temperature with the same repaint rate. If the panel degrades with no sun,
  the clean is the cause and heat is only an accelerant.
- **Heat alone.** Warm a stationary device to a known temperature, hold the
  frame, then run a `HALF`. Photograph before and after. No oven above whatever
  the vendor sheet allows.
- **`0x5A` direction.** Same warm setup, one `HALF` with `0x5A` and one with a
  value matching the real temperature. If the honest value looks better, item 3
  is settled.
- **Vibration.** Bench-shake with no sun and a fixed frame. Torn lines mean
  connector; even paleness means it was never vibration.
