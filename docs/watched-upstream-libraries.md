# Watched upstream libraries

Libraries not in our dependency tree today, but close enough to our problems
that a session should check them before assuming a problem is unsolved or a
technique is novel. Read this list before deep firmware panel-driver work.

## FastEPD (bitbank2)

<https://github.com/bitbank2/FastEPD>

What it is: an ESP32 library for driving parallel e-ink panels, Apache 2.0,
written by the author of OneBitDisplay, bb_spi_lcd and bb_epaper. Built as an
alternative to EPDiy.

Why watch it: it names our reference board explicitly —
`BB_PANEL_LILYGO_T5PRO` in `src/FastEPD.h` is the LilyGo T5 S3 4.7" Pro, the
same board `project_reference_device_x4pro` tracks. It is actively
maintained (commits as recent as the day this doc was written, 2026-09-01).

**Full findings from the 2026-09-01 research pass are in
[`fastepd.md`](fastepd.md)**, not duplicated here — this entry is the pointer,
that doc is where the content lives. Headline items: it surfaced an
already-merged sunlight-fix setting in our own tree that defaults off
(`fadingFix`, `src/CrossPointSettings.h:376` — now T-584/T-585 in
`docs/TODO.md`), a documented ESP32-S3 parallel-bus erratum relevant to T-576,
and a gray-matrix design worth reading before any tunable-waveform work.

Not evaluated: whether FastEPD fits our HAL, whether its parallel-bus
assumptions match the T5 S3 Pro's actual wiring, whether it is faster or more
correct than our own driver. This is a library to study, not one to adopt.

**Check for new commits and re-read relevant source before any T5 S3 Pro
panel-driver, partial-refresh, or grayscale work** — this doc is a pointer,
not a snapshot; the library moves.
