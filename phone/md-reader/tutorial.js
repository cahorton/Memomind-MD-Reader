// Built-in tutorial notes, shown as a folder in the vault until hidden.
// Each <!-- pagebreak --> starts a new glasses page, so every page is one
// short, self-contained step and practice links share a page. Check page fit
// with a wider font: SIM_CHAR_WIDTH=13 in test/host_sim.c. Keep the
// wording in step with what the reader does; test/render.test.mjs checks that
// every link resolves within this folder.

export const TUTORIAL_FOLDER = 'MD Reader Tutorial';
export const TUTORIAL_START = `${TUTORIAL_FOLDER}/1 Start here.md`;

const BREAK = '\n<!-- pagebreak -->\n';
const pages = (...parts) => `${parts.map((part) => part.trim()).join(`\n${BREAK}\n`)}\n`;

// Rule for this text: every action that changes something sits on the same
// page as how to undo it. A page the reader cannot see (display off, a note
// left behind) is no help.
const startHere = pages(`
# Start here
Welcome to MD Reader. To turn the page, look down a little and hold it for a moment, then look back up.
Try it now.
`, `
## Turning pages
Well done. Each look down turns one page. To go back a page, look up the same way.
Come back to level between pages.
`, `
## If the display goes dark
Shaking your head "no" turns the display off. To turn it back on, nod a few times or press any button on the glasses, then look straight ahead while it counts down to recentre.
`, `
## The tilt guide
The line on the right edge is the tilt guide. The small square is your head. A page turns once the whole square is past one of the two ticks.
`, `
## Changing the angle
On the phone, under Head control, **Tilt to page** sets how far you tilt. The ticks move with it.
`, `
## Which way did it go?
After each page turn, a thin arrow shows the way: down below the text, or up above it.
`, `
## Pausing
A single tap on the glasses pauses head scrolling. Tap again to turn it back on.
`, `
## Straight ahead
If paging feels off, look where you like to read. Tap once, wait a second, then tap again: the first tap pauses, the second resumes, centred on where you look. (Two quick taps open a link instead.)
**Recentre tilt** on the phone does the same.
`, `
## Links
Underlined words are links. Turn your head **right** to box the next link, **left** for the previous one.
Nod once, down and back up, to open the boxed link.
`, `
## Coming back
Long-press the glasses to return from a link.
With nothing to return to, a long press counts down to exit instead: let go to cancel.
`, `
## Practice
Move the box between these two, then nod to open one. Long-press to come back:
- [[2 Links and going back|Links and going back]]
- [[3 Continuous scrolling|Continuous scrolling]]
`);

const linksAndBack = pages(`
# Links and going back
Move the box through these, then nod to open one. Long-press to come back:
- [[1 Start here|Start here]]
- [[1 Start here#Pausing|Pausing]]
- [[3 Continuous scrolling|Continuous scrolling]]
- [[4 Display and brightness|Display and brightness]]
`, `
## Going back
A long press returns you to the same page, with the same link boxed. With nothing to return to, it counts down to exit: let go to cancel.
`, `
## Good to know
After a link opens, the page waits 2 seconds before it can turn, so the end of your nod does not page.
`, `
## On the phone
The phone shows the same note. Tap any word there to jump the glasses to that spot. Look up to page back.
`, `
## Next
Long-press to go back, or open [[3 Continuous scrolling|Continuous scrolling]].
`);

const stretches = [
  'Hold your look down and pages keep coming, one per pause.',
  'Come back to level and the scrolling stops.',
  'Hold your look up to scroll back the same way.',
  'A longer pause gives you more time to read each page.',
  'The arrow below the text means you went forward.',
  'If the square on the tilt guide stays past a tick, you keep scrolling.',
  'Tap the glasses to pause everything for a while. Tap again to carry on.',
  'Last one. Look up for a moment to scroll back, then come back to level.',
];

const continuous = pages(`
# Continuous scrolling
This part has several pages, to practise scrolling through.
`, `
## Turning it on
On the phone, under Head control, turn on **Keep scrolling while I look up/down**, and set **Pause between pages**. Turn it off again for one page per look.
`, `
## How it works
Hold your look down and pages keep turning, one each pause. Hold it up to scroll back. Come back to level to stop.
`, `
## Auto-scroll
**Start auto-scroll** on the phone turns pages on a timer, wherever you look. Tap the glasses to stop it.
`, ...stretches.map((line, index) => `
## Stretch ${index + 1}
${line}
`), `
## The end
Look up to scroll back, or open [[4 Display and brightness|Display and brightness]] or [[1 Start here|Start here]].
`);

const display = pages(`
# Display and brightness
Shake your head "no" a few times, quickly, and the display turns off. To turn it back on, nod a few times or press any button.
`, `
## Waking up
When the display comes back on, look straight ahead. It counts down from 2, then takes where you are looking as level.
`, `
## While it is off
Nothing scrolls while the display is off. If nodding does not wake it, press any button on the glasses.
`, `
## Brightness
**Hold glasses brightness** on the phone stops the glasses dimming as you look around. Turn it off to go back to automatic brightness.
`, `
## If something does not respond
**Glasses log**, at the bottom of the phone app, lists what the glasses notice, such as each nod and how fast it was.
`, `
## Finishing
When you are done, tap **Hide tutorial** under the vault list on the phone. **Imported files** can bring it back.
Back to [[1 Start here|Start here]].
`);

export const TUTORIAL_NOTES = Object.freeze({
  [TUTORIAL_START]: startHere,
  [`${TUTORIAL_FOLDER}/2 Links and going back.md`]: linksAndBack,
  [`${TUTORIAL_FOLDER}/3 Continuous scrolling.md`]: continuous,
  [`${TUTORIAL_FOLDER}/4 Display and brightness.md`]: display,
});
