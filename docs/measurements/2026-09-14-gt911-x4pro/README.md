# GT911 and sampler captures, X4 Pro, 2026-09-14

Raw evidence behind [`../../input-gestures.md`](../../input-gestures.md), "What
the controller and the loop actually do". Kept because the prose quotes numbers
that cannot be re-derived without it, and because a later reader has to be able
to check a claim rather than trust it.

Device: X4 Pro, serial `b8:1f:3f:d4:89:bc`. Firmware: the branch that became
`develop` `292dd6c9`. Sampling 5 ms unless a file says otherwise.

Capture format: `us,status,int,repeats[,x,y]`, run-length encoded on the
(status, INT) pair. `status` bit 7 buffer-ready, bit 4 the capacitive home key,
bits 3..0 the contact count. `int` is the level on GPIO10. `-1` coordinates mean
the frame reported no contact.

**Read the verdict column before using a file.** A dense capture overruns USB CDC
and whole blocks of lines go missing; a dropped block reads as a silent
controller, which is the exact false negative these captures exist to rule out.

| file | what | verdict |
|---|---|---|
| `cap1-idle-clear.txt` | idle, 3 s | complete |
| `cap2-hold-clear.txt` | intended key hold, nothing registered | complete, empty |
| `cap3-tapping-clear.txt` | -- | **empty: the command never reached the device** (leading `\n` behind `main.cpp`'s `peek() == 'C'` gate) |
| `cap4-idle-recheck.txt` | idle, 2 s, after the host tool was fixed | complete |
| `cap5-tapping-clear.txt` | free tapping, key and glass, 8 s | **complete, 1601/1601 -- the load-bearing capture** |
| `cap6-hold-clear.txt` | intended key hold | **LOSSY, ~587 samples missing. Conclude nothing from it.** |
| `cap7-keyhold-clear.txt` | intended key hold | **LOSSY, 95 rows missing. Conclude nothing from it.** |
| `cap8-keyhold-coords.txt` | key hold, 8 s, with coordinates | complete, and **empty** -- a held key produced no frame at all |
| `cap9-glass-coords.txt` | glass tapping, 8 s, with coordinates | complete, 1601/1601 |
| `cap10-key-noclear.txt` | `noclear`, key tapped throughout | complete, 1601/1601 |
| `cap11-delayclear.txt` | first `delay2000` attempt | complete, and **empty** -- the tap happened before the capture started. Kept because it is why the arm phase exists |
| `cap12-delayclear-armed.txt` | `delay2000` with the arm phase: key press latched, held 2 s, acknowledged once, then watched | **complete, 1601/1601 -- answers open question 5** |
| `loopgap-home-idle.txt` | `CMD:LOOPGAP`, Home, idle | -- |
| `loopgap-map-open.txt` | `CMD:LOOPGAP` across `CMD:GOTO_MAP` | -- |
| `loopgap-map-redraw.txt` | `CMD:LOOPGAP` across one map `redraw` | -- |

`cap6` and `cap7` are kept rather than deleted because they are why the
instrument grew flow control and a loss check, and because a later reader who
finds them quoted somewhere needs to see the verdict next to them.

**`cap12` is the one that closes the mechanism.** It is the first capture in
which the latched frame is a *gesture's own edge*: `0x90`, the key press, held
unchanged for 402 samples (2.01 s) after the finger was long gone. The single
acknowledgment at 2,005,001 us is followed 10 ms later by a fresh `0x80` -- so
the controller **does** re-report current state after a late clear, within one
frame period.

**What `cap10` does and does not show.** It shows the controller holding one
frame for 7.99 s of an 8.00 s capture with nothing else getting through. It does
**not** show a gesture's own edge being the frame that latched: the latched frame
is a contactless `0x80` that arrived 10 ms into the capture, before any tap. The
blocking is measured; "the first edge of the gesture latches" is inference.

**A `LOOPGAP` gap has no cause attached.** The histogram records the interval
between input samples and nothing about why. The attribution of 4.34 s to opening
the map and 2.80 s to a redraw rests on the test protocol -- read the report,
do exactly one thing, read it again -- not on anything in the data. Any other
blocking command in between would land in the same bucket.

Reproduce with `tools/touchlog.py` in the parent repo.
