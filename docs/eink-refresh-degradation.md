# The panel refreshes badly in daylight, and the map never cleans it

Two field reports on the X4, both from the maintainer, both unmeasured.

**Report 1, 2026-08-22, motorbike, sunny, engine running:** large parts of the
panel came back pale after a refresh -- big white patches, weak contrast, at
times nearly unreadable. It recovered as the marker moved and more refreshes
landed. Three suspects named at the time: direct sun, heat, vibration.

**Report 2, earlier, walking in a city, device in hand:** a refresh landed while
a **thumb covered part of the screen**. Almost the whole panel came out white --
**except the patch under the thumb**, which developed correctly.

Report 2 is the important one, because it is a natural stencil experiment and it
rules most of report 1's suspects out. See "The thumb stencil" below. Short
version: **the primary suspect is light falling on the panel during the
refresh**, the missing clean is a separate real defect, heat is a minor
aggravator, and vibration is out.

**Nothing here is measured.** The device was lost on report 1's ride
(`docs/mounting.md` in the parent repo), so every hardware check is blocked until
a replacement arrives.

## The thumb stencil

The thumb picture settles more than any of the timing work could, and the
argument is short.

**A waveform is global. It cannot produce a pattern.** The controller has one
temperature register and one internal sensor reading for the whole panel
(`Ssd1677Driver.cpp:159-165`, `:274-275`). Whatever waveform a refresh selects,
every pixel gets the same one. So **any result that varies across the glass in
the shape of an object comes from something acting locally on the glass** -- not
from the mode, not from the temperature register, not from the missing clean.

Two things a thumb does locally: it **shades** and it **warms**.

Warming is the wrong answer, twice:

- **Direction.** The `HALF` clean already tells the panel it is hot
  (`0x1A = 0x5A`, below), so a warm patch is the region *closest* to what the
  waveform assumes and should look best -- but under a thumb in daylight the sun
  is off that patch, so the exposed glass is the hotter part, not the cooler one.
  The thermal story predicts the exposed area developing best. It came out white.
- **Sharpness.** Heat spreads sideways through glass and film over millimetres
  and tens of seconds. A `HALF` takes 1,684 ms (`docs/refresh-modes.md`). There
  is no time to build a thumb-shaped thermal edge, and conduction would smear it
  anyway. The reported edge was the thumb.

Shading leaves one mechanism, and it is standard display physics rather than
anything e-ink-specific: **an amorphous-silicon TFT is a photoconductor.** Light
reaching the backplane generates carriers in the channel and the off-state
leakage of every pixel transistor rises by orders of magnitude, so a pixel cannot
hold its drive voltage for the length of the waveform. Under-driven pixels do not
develop; they stay at, or fall back to, white. Shade the transistor and it holds.
That is exactly a white panel with a correct thumb-shaped patch.

**Confidence: mechanism strongly indicated, not verified here.** Marked open:

- **No primary source cited yet.** TFT photoconductivity is textbook, but this
  project's rule is a vendor page or a datasheet, not a recollection. Needs an
  E Ink or Good Display application note, or a paper, before it is stated as
  fact.
- **Whether GDEQ0426T82 carries a light-blocking layer is unknown**, and the
  panel datasheet is the same document T-505 already needs for the UV rating.
  The second reason to get it.
- **Sunlight versus visible light versus UV** is unseparated. Report 2 was a city
  walk, so ambient daylight was enough; nothing says direct sun is required.

### What the sources say (researched 2026-08-22)

The mechanism above was reasoned out from the thumb before any of this was read.
It holds up, and it is a **known, named problem** in this industry rather than
anything peculiar to the X4.

**Other people have reported the exact same stencil.** A MobileRead thread on
e-readers in direct sunlight
(https://www.mobileread.com/forums/showthread.php?t=27424): "if I leave the
shadow of my hand over the reader whilst I change page I get a nice hand-print in
the display", "the text on the part that was exposed to sunlight is noticeably
lighter", "it only takes about 10 minutes before the text is almost too faded to
read", and it is reversible -- "the screen rapidly returns to normal when moved
out of the sun". Sony PRS-505 and others, so consumer readers with Carta-class
film, not just bare modules. **This is the closest thing to a replication of
report 2 that exists without a device.**

**A module vendor states the mechanism in one sentence.** Pervasive Displays, an
E Ink licensee, on sunlight readability
(https://pervasivedisplays.freshdesk.com/support/solutions/articles/72000632262):
"Placing an EPD under direct sunlight can lead to degraded image quality **while
refreshing** the E-ink screen as the strong light could cause **current
leakage**." Same page: the displays are "designed for indoor or semi-outdoor
environments".

**A patent describes it in an EPD, and shows the fix is in the panel.**
US11171241B2, "TFT substrate", Japan Display Inc, priority 2018-01-19
(https://patents.google.com/patent/US11171241B2/en): light on the display surface
"may reach the pixel transistors", and "the intense light entering the pixel
transistors generates a photo leakage current due to a photoconductivity effect,
and may cause a malfunction of the pixel transistors". The remedy claimed is
**light-blocking films over the gate/drain regions** of each pixel transistor.

Two consequences of that last one, and they matter to this project:

- **Panels differ.** A backplane with light shields is far more sun-tolerant than
  one without. Whether GDEQ0426T82 has them is unknown, and so is whether an X4
  Pro or X3 panel does. **This becomes a device-selection criterion** -- T-514.
- **Nothing on our side of the glass can fix a panel that lacks them.**

**Photo leakage is driven by visible light, not only UV.** The classic
measurements are of a-Si:H TFTs under **backlight** illumination -- CCFL and white
LED, no UV involved (Molecular Crystals and Liquid Crystals 499(1),
https://www.tandfonline.com/doi/full/10.1080/15421400802619917). Dark off-state
leakage is around 1e-13 A; illuminated it runs about 1e-10 A, three orders up, and
scales with light intensity. **So a UV filter is the wrong tool for this failure**
-- and that is exactly what a hobbyist found: a UV-stabilized acrylic sheet over
an outdoor e-paper watch left the black text faded and unreadable, and he never
found a fix (https://jeonlab.wordpress.com/2022/03/12/e-paper-display-problem-under-the-sunlight/,
plus two commenters with the same problem). What did work for another builder was
an **IR window film**, which cuts broadband flux -- and made the display "a little
darker" (https://forum.arduino.cc/t/e-paper-display-has-problems-under-sunshine/1010179).
Attenuating everything works; filtering UV alone does not.

**There is a second, irreversible failure that is not what we saw, and we should
not collect it.** Both module vendors warn that prolonged strong light **dries the
pigment out permanently**: "charged particles may dry out under prolonged intense
light exposure, losing activity and becoming unable to refresh. This situation is
irreversible" (Waveshare, https://docs.waveshare.com/5inch_e-Paper/FAQ, which also
says outdoor use voids warranty), and Good Display's own precautions page says to
keep e-paper "out of direct sunlight" and to "take ultraviolet protection
measures" (https://www.e-paper-display.com/news_detail/newsId=53.html). Our panel
recovered, so this is not what happened -- but a device that lives on a bike in
the sun is being aimed at it. This is the same risk T-505 tracks.

**Outdoor e-paper products exist, and this is what they spend to get there.**
Papercast and Visionect bus-stop signage: laminated or tempered front glass with
integrated UV protection filtering "over 98%" of UV, optically bonded to the
panel, in a ruggedized enclosure -- because an unprotected EPD "will fail very
quickly outdoors" (https://www.visionect.com/blog/e-paper-extreme-weather/,
https://www.papercast.com/product/unpacking-digital-signage-the-ruggedized-enclosure/).
Note what that buys and what it does not: their UV stack addresses the
irreversible damage. Their sunlight *refresh* quality comes from the panel and the
enclosure, not from a film we can stick on an X4.

**And the vendor of our exact panel gives a number we are ignoring.** Good
Display's GDEQ0426T82 page (https://www.good-display.com/product/957.html):
operating temperature **0-50 C**, storage -25-60 C, and "when using Fast Refresh
and Partial Refresh, it is recommended to perform a Full-Screen Refresh **after
every five consecutive operations**". We do zero full-screen refreshes for the
whole map session (BUG-021 in the parent repo). Five is the vendor's number for
ghosting, not for this light problem; a ride-tolerable N will be larger and the
reason has to be written down. Their general "refresh interval at least 180 s"
guidance explicitly excludes partial-refresh products, so it does not bind us.
Their other timing rule -- refresh at least once every 24 hours, weekly at worst
(https://www.e-paper-display.com/news_detail/newsId=53.html) -- is aimed at
tri-color panels; a device that sleeps with a fresh sleep screen and wakes to a
clean satisfies it either way.

**Confidence after the reading:** the mechanism is **sourced** -- a vendor
sentence, an EPD patent, the TFT literature, and independent user reports of the
same hand-shaped stencil. What is still **open** for our device specifically:
whether GDEQ0426T82 has light shields, at what illuminance ours starts failing,
and how much of report 1's paleness was this versus the missing clean.

### What follows from it, and it is uncomfortable

**No firmware change fixes a photoconductive backplane.** Waveform tuning, a
ghost-clear interval, an honest temperature -- all of it operates on a panel that
cannot hold charge while lit. The fixes that remain are physical or procedural:

- **Shade the panel while it refreshes.** On a mount that means a hood or visor
  over the screen -- an additive part, which is allowed (parent `CLAUDE.md`,
  "Never open a device"). This becomes a mounting requirement next to the tether
  that report 1's loss already made one. A hand over the glass is the free
  version and it demonstrably works: it is what produced the stencil.
- **If a film, then a broadband one, not a UV one.** UV filtering does not touch
  photo leakage (it is driven by visible light) and a UV-stabilized sheet failed
  for one builder outright; an IR window film worked for another at the cost of a
  darker screen. Any film trades away the reflectivity the product is sold on, so
  a hood beats a film wherever geometry allows.
- **Refresh twice.** A second pass re-drives what the first left undriven, which
  is what marker movement was doing by accident in report 1. Cheap, partial, and
  worth having whatever else is true.
- **Do not promise what the panel cannot do.** The public site's comparison table
  says the screen is readable in direct sun (`web/src/compare.html:23`), and that
  is still true -- reflective ink reads better the brighter it gets. **Reading is
  not refreshing.** If this holds up, the honest claim is "reads in sun, wants
  shade while it updates", and the table needs revisiting. Not yet: one field
  report and one stencil observation are not a measurement.

## The map never cleans the panel: a real defect either way

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

## Heat: a minor aggravator, and the code makes it worse in one place

Ranked below light because it cannot explain the thumb, but the code detail below
is real and worth fixing on its own. Two things stack on top of the missing
clean.

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

**Settled 2026-08-22, from the vendor's own product page**
(https://www.good-display.com/product/957.html): GDEQ0426T82 operating
temperature is **0-50 C**, storage **-25-60 C**, and the quoted refresh times are
measured at 25 C. So the panel has a stated ceiling of 50 C, and a dark device in
a mount in direct sun is a plausible way past it. What the OTP waveform table does
above 50 C is still open -- the datasheet, not the product page, would say.

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

## Vibration: out

Report 2 removes it: a thumb on the glass does not stop a city walk from shaking
less than a motorbike, and the failure appeared with no engine at all. Kept here
for the mechanism, not as a live suspect. Microcapsule ink is
suspended in a solid binder; ordinary vibration does not move pigment. The two
paths that could matter are both mechanical-electrical, not optical:

- **FPC/connector disturbance.** A marginal panel flex under vibration corrupts
  SPI writes. That looks like torn lines, shifted blocks or garbage -- not soft,
  even paleness. Not what was seen.
- **Boost-rail sag.** The panel's charge pump needs its high gate/source rails
  to drive ink. Brownout under load would weaken the whole frame. Plausible in
  principle, but the X4 exposes no way to measure it, and nothing may be wired
  into the charge path or the cell (parent `CLAUDE.md`, "Never open a device").

So: the connector case would look different from what was reported, and the
stencil rules the rest out.

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

0. **A sun hood over the screen** on any mount, and a shade-it-with-your-hand
   habit before a refresh that matters. If the stencil result holds, this is the
   only thing that addresses the primary cause at all, and it is a 3D-printed
   clip-on part -- additive, so allowed. Belongs in `docs/mounting.md` (parent
   repo) as a requirement, not a nice-to-have.
0b. **Refresh twice when it matters.** A second pass over the same frame
   re-drives pixels the first left undriven. This is what marker movement did by
   accident. Costs one more waveform; no new hardware, no measurement needed to
   justify it.
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

Order matters. 0 and 0b are the only entries that touch the light mechanism, and
0b needs nothing measured first. 1 and 4 are worth having whatever the light and
temperature stories turn out to be, because the missing clean is a bug on its own
in the dark at room temperature.

## What would settle it

Every item needs a device. Blocked.

- **The stencil, repeated on purpose** -- do this one first, it is minutes. Tape a
  cardboard cutout (a shape nothing else could produce: a letter, a cross) over
  the panel, put the device in daylight, run one `HALF` on a known frame,
  photograph. If the cutout appears, light is confirmed and everything else is
  secondary. Then repeat in shade at the same temperature: the cutout must
  vanish. Then repeat under glass or a UV filter, to separate visible light from
  UV.
- **Light versus heat, separated.** Warm the panel in the dark with a heat source
  to the temperature the sun produced, and refresh. A failure there is thermal; a
  clean refresh there plus a failure in cool daylight is optical.
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
