# MD Reader for MemoMind glasses

An Obsidian-style Markdown reader. Notes live on the phone and are read on
the glasses: head tilt pages through the note one screen at a time, and head
turns pick links. Links between notes (`[[wikilinks]]` and `[text](note.md)`)
can be followed and retraced.

## Controls

| Input | Action |
| --- | --- |
| Look **down** past the threshold | Next page (scrolls down) |
| Look **up** past the threshold | Previous page (scrolls up) |
| Keep looking down/up (continuous mode) | Keeps paging, one page per *pause* |
| Turn head **right** / **left** (a quick flick) | Highlight the next / previous link on the page |
| **Double-click** | Open the highlighted link |
| **Long press** | Back to the previous note (with nothing to go back to: 3 s exit countdown, release to cancel) |
| **Single click** | Stop auto-scroll if it is running; otherwise pause or resume head control (resuming recentres "neutral" on your current pose) |
| **Start auto-scroll** (phone button) | A page every *pause*, wherever you look; stops at the end of the note, on a tap, or when another note opens |
| Ring up/down, page up/down | Previous / next page |
| Ring left/right | Previous / next link |
| Ring back | Back (or exit) · Ring home: exit |

Head tracking details (all adjustable in the phone plugin's **Head control** panel):

- **Tilt to page** (default 12°): pitch, measured from your neutral pose.
  There is 3° of hysteresis, so a tilt hovering near the line does not flicker.
- **Hold before paging** (default 0.3 s): a quick glance does not turn the page.
- **Keep scrolling while I look up/down** (continuous mode, default off): off,
  each look turns exactly one page; look back, then again for the next. On,
  holding the look keeps paging, one page per **Pause between pages**
  (1–10 s, default 4 s), so you can read each page while you hold.
- **Scroll arrow:** after every page turn a skinny chevron shows which way it
  went: pointing down below the text, or pointing up above it. It is bright
  for 2 s, then stays dim until the next turn, so you can still tell after
  looking away.
- **Neutral follows you.** It is first taken when head control starts (and on
  each single click), but it does not stay fixed:
  - when you come back from a page turn, wherever your head settles becomes
    the new neutral, so the return swing never pages the other way;
  - outside a tilt, neutral slowly follows posture changes (over ~5 s);
  - a tilt held for 2.5 s past its page turn is taken as your new reading
    pose. This fixes a start taken while looking down at the phone: you get at
    most one stray page, not endless paging. If you go back to the old pose
    within a few seconds, it is restored without paging. Continuous mode keeps
    this only until neutral is confirmed: by a single click, by coming back
    from any page turn, or by such a re-centre. After that, a held look
    scrolls for as long as you hold it.
- When a phone setting change reaches the glasses, they show
  **Settings updated**.
- **Hold glasses brightness** (default on): stops the glasses' automatic
  brightness from dimming the display while MD Reader runs. **Brightness**
  `keep current` holds whatever level the glasses had; 1–10 sets a fixed level.
  The glasses release the hold when MD Reader stops.
- **Turn needed for links** (default 120, raw gyro units): a flick must exceed
  this for two samples in a row. The return swing is ignored until the head
  settles, so a left-and-back turn moves one link, not two.
- Link turns are ignored while a tilt is active, so looking up and to the side
  never does both.
- An optional **tilt guide** in the right margin shows where your head is
  relative to the two thresholds (the ticks). The marker brightens when the
  head is past a threshold.

On the glasses, links are underlined. The highlighted link is drawn inverted.
The status line shows the link count or a notice, plus `page / total` (or
`page  percent%` until the end of the note has been reached).

## How it works

```
phone (Web plugin, JS)                          glasses (.gmp, C)
 vault: .md files / vault .zip                   pagination with the firmware font
 Markdown → display text + link byte spans       underline + selection rendering
 link resolution, back history          ⇄ BT ⇄   head tracking (pitch paging, yaw links)
 serves 8 KB text windows on request             buttons / ring
```

- **Phone** (`phone/md-reader`): imports notes through the Host file picker,
  either several `.md` files or a zipped vault (folder paths kept). It renders
  Markdown to plain text the glasses font can show:
  - headings keep their `#`
  - lists use `·`, tasks `[ ]`/`[x]`, quotes `|`, callouts `NOTE: …`
  - emphasis, HTML, front matter and `%% comments %%` are stripped
  - images become `[image: alt]`

  Import copies each file into MD Reader's own storage. **Remove** in the
  phone plugin deletes only that copy; the original file stays where it was.
  Importing a file with the same name as one already imported replaces the
  older copy, so re-adding an edited note or a fresh vault zip updates it.
- **Folders:** the MemoMind app's file picker can only pick files, not a
  folder. To bring in a whole vault with its folders, compress the folder and
  add the zip. On an iPhone, in Files, long-press the vault folder and choose
  **Compress**. On a Mac, right-click the folder and choose **Compress**. The
  phone plugin then lists the vault as folders you can browse: sub-folders
  first, then notes, both alphabetical (numbers in natural order, so `s2`
  comes before `s10`). The breadcrumb jumps back up, and the search box looks
  through every folder. A zip whose notes all sit in one top folder (a zipped
  `ObsidianVault/`) opens straight inside it.

  Links resolve like Obsidian: by note name (preferring the same folder), by
  relative or vault path, with `#heading` targets and `|aliases`. The phone
  owns the back history and restores your place (page and highlighted link)
  on back and after a restart.
- **Glasses** (`glass/md_reader`): requests the text in 8 KB windows and lays
  pages out locally with the Host's font measurement, so pages match the
  display exactly. Head tracking reads the raw IMU locally every 40 ms, so
  paging never waits on Bluetooth.
- The wire protocol (channels `0x4D44` phone→glasses and `0x4D45`
  glasses→phone) is documented at the top of `phone/md-reader/md-protocol.js`
  and `glass/md_reader/md_reader.c`.

## Build, test, run

```bash
npm test
```

This runs the Markdown/link/protocol unit tests, plus an end-to-end test. The
end-to-end test compiles the real glasses C code natively against a fake Host
(`test/host_sim.c`, with AddressSanitizer and UBSan) and drives it with the
real phone controller. It covers paging by tilt (threshold, dwell, repeat),
link picking by head flicks, follow/back, heading links, font changes and
exit.

```bash
npm run studio
```

This copies both plugins into `../plugin-open-platform`, builds the `.gmp`
and `.mmpkg`, and zips `samples/vault` into `samples/vault.zip`. MemoMind's Web
SDK (`phone/md-reader/vendor/gm-plugin-web-sdk.esm.js`) is not part of this
repo; the script copies it from the SDK checkout, so you need the
[MemoMind Plugin Open Platform](https://open-platform.memo-mind.com) SDK next
to this folder (or pass `--sdk`). Then:

1. In Desktop Studio, click **Refresh repository** on both sides.
2. Pick **MD Reader** as the phone plugin and **MD Reader** as the glasses plugin.
3. Add the sample notes and use Studio's head-gesture and button controls.

`npm run studio -- --remove` undoes it.

## Not yet verified on hardware

- The IMU axis signs are inferred from the SDK's `imu` example: pitch rises when
  you look up, and `gyro_x - gyro_y` is negative when you turn left. If either
  is reversed on the glasses, use **Swap up/down** / **Swap left/right**.
- Default thresholds are guesses until tried on a real head.
- Grey levels (`0xDB` dim text, `0x92` guide) assume the 8-bit LVGL colour maps
  to brightness. Check them on the display.

## License

MIT, see [LICENSE](LICENSE). This covers the code in this repository only.
MemoMind's SDK, which the build copies in from your SDK checkout, stays under
the [MemoMind Open Platform Terms](https://open-platform.memo-mind.com/terms).
