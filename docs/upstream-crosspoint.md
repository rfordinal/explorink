# Keeping up with upstream CrossPoint: review, decide, move a pointer

**Rule, maintainer, 2026-09-08.** We do **not** blind-merge upstream. Every so
often we **read what is new**, decide per item whether we take it as a
**cherry-pick**, keep it as an **idea**, or **skip** it, and then move a pointer
that records **which upstream commit we last reviewed**. The SDK is the other
track and has its own rules: [`freeink-sdk-fork.md`](freeink-sdk-fork.md).

The pointer lives in [`upstream-reviewed.sha`](upstream-reviewed.sha) and one
command lists everything landed since:

```
scripts/upstream_review.sh
```

## Why not a plain merge: the numbers, 2026-09-07

| | Value |
|---|---|
| Our `develop` ahead of `upstream/develop` | 741 commits |
| Our `develop` behind | 140 commits |
| Last shared commit | `1a7f5a9e`, 2026-07-31 |
| Our divergence since | 372 files, +86,066 / -12,762 |
| **Files conflicting on `git merge upstream/develop`** | **46** |
| Measured between | `7ff9e5e8` (our `develop`) and `7db14a01` (`upstream/develop`) |

A conflict count means nothing without both tips, so they are part of the number.

The conflicts are not in the reader code we never touch. They are in the files
both sides rewrote: `platformio.ini`, three workflows, `scripts/git_branch.py`,
`HalPowerManager`, `HalDisplay`, `HalSystem`, `GfxRenderer`,
`MappedInputManager`, `SettingsList`, `CrossPointSettings`, `ActivityManager`,
`HomeActivity`, `SettingsActivity`, `SleepActivity`, `OtaUpdateActivity`, all
three themes, three translation files, and `SettingsPage.html` as a
modify/delete because we deleted it.

**Cherry-picking the device support is not cheaper by conflict count**, only by
scope: `bbca4886` (x4pro + papermono, 54 files) touches 23 files we have also
changed, the same hot ones.

## The merge that is worth doing: an integration branch, stripped first

Maintainer's proposal, 2026-09-08, and it is the right shape for the day we do
take a large batch: branch an **integration branch**, **delete on our side what
we are never going to carry**, then merge upstream into it and **resolve every
conflict in favour of our own clean code**.

What that buys, measured against the same 140 commits by classifying every
commit's paths:

| | Commits |
|---|---|
| Touch **only** paths a strip would delete (EPUB, reader activities, dictionary, OPDS/KOSync, translations, built-in fonts, TXT/XTC) | **22** |
| Mixed: partly in strippable paths | **51** |
| Untouched by any strip | **67** |

**These three numbers are only as good as the path list.** They come from
classifying every commit's touched paths against: `lib/Epub/`,
`src/activities/reader/`, `lib/I18n/translations/`, `lib/EpdFont/builtinFonts/`,
`src/activities/dictionary`, `src/activities/opds`, `lib/Dictionary`,
`src/network/KOReader`, `lib/KOReaderSync/`, `src/activities/settings/Opds`,
`src/activities/settings/Font`, `lib/Txt/`, `lib/Xtc/`, `lib/expat/`,
`lib/MiniBidi/`, `lib/InflateReader/`, `test/epubs/`, `test/hyphenation`. A
narrower strip moves them toward 67 and a wider one away from it, so the list is
part of the claim.

So the strip removes about a sixth outright and thins half of the rest. **It does
not make the merge free** -- 67 commits are genuinely in our own surface -- but it
turns every future modify/delete into "keep deleted", and it is on the roadmap
anyway (parent `docs/firmware-cleanup-plan.md`; this repo's `CLAUDE.md` says the
fork strips the e-reader stack over time).

Order matters: **strip first, merge second.** Stripping during a conflict
resolution is how you delete something upstream had just fixed for a reason you
never read.

## One thing this pass got wrong about itself

The pass reported the `sticky` env break as three weeks nobody noticed. **Half of
that is false.** The same defect on the three release envs was found 2026-09-02,
written into [`build-environments.md`](build-environments.md) and tracked as T-240
in the parent repo. What was genuinely unreported is the `sticky` half: that pass
was hunting a release binary and never built the S3 env.

Kept here because the session that found the fix also wrote the wrong story about
it, and the story is the part that travels. **Read the topic doc before claiming a
finding is new.**

## The review pass, step by step

1. `git -C firmware/explorink fetch upstream`
2. `scripts/upstream_review.sh` -- prints the pointer and every commit since.
3. Read subjects, open what looks relevant. Classify each into **take**,
   **idea** or **skip**, and write the reason for anything not obviously either.
4. Cherry-pick the takes onto their own branch, build every env, run host tests.
5. Add a row per decision to the log below, move the pointer, commit the two
   together.

**A pass with no takes is a valid pass.** Moving the pointer and writing "nothing
we want" is the point of having a pointer.

## Decision log

Newest pass on top. `take` = we want the code, `idea` = we want the thought and
will write our own, `skip` = not ours.

### Pass 1, 2026-09-07/08, up to `7db14a01` (2026-09-06)

Reviewed 140 commits. Nothing cherry-picked yet: this pass produced the
classification and the SDK bump on its own branch.

| Verdict | Commits | Why |
|---|---|---|
| **take**, device support | `bbca4886` x4pro + papermono, `f021d1e5` X4 Classic board, `4e619035` new X3 display + battery drain, `9b1fb712` GPIO13 guard for C3 only, `218dc6aa` USB MSC on X4 Pro, `f0c65506` X4 Pro power-latch through deep sleep, `e00f5958` cross-chip install guard | Every Xteink a buyer can order today is an S3, and the cross-chip guard is exactly the mistake one repo shipping two chips makes |
| **take**, power and reliability | `4a679d5d` SD off on deep sleep, `e37164ca` cold-boot power-button, `cdd89b1d` wake detection, `53232eef` watchdog resets as panics, `72121293` empty watchdog reports, `8bc1603e` USB detect via charge state | A hang or a lost report in the field is our worst failure mode |
| **take**, memory and flash | `c484dc72` built-in fonts -323 kB, `93b6fe11` glyph arena under heap pressure | 380 kB is the ceiling and flash is 61.5 % used |
| **idea**, not the code | `70faa29d` light-sleep idle + refresh downclock, **reverted** by `81e19270` | Upstream took it back out, so the "3.2x reading time" number describes code that no longer exists. Read both before quoting either |
| **take deliberately, own pass** | `7db14a01` pioarduino 55.03.311, `e31dcbb0` don't recompile Arduino IDF libs, `255bab31` UTF-8 `platformio.ini` in `git_branch.py` | A toolchain move changes every env at once and needs its own size and heap comparison. `e31dcbb0` touches the shared prebuilt directory `build-environments.md` warns about |
| **skip** | The bulk: EPUB layout, dictionaries, RTL and Arabic shaping, keyboard layouts, the FUI conversion, the touch reader toolbar, the control center, dark mode. 206 file-touches in translations, 118 in reader activities, 72 in settings, 64 in built-in fonts | Upstream's product, not ours |

**The honest cost of the skips:** those commits edit files our map work also
edits, so the conflict surface grows whether we take them or not. 140 becomes 200
next month. The choice is not merge or don't, it is pay it in one planned pass or
pay it in every future one.
