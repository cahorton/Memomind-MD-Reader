// Built-in tutorial notes, shown as a folder in the vault until hidden.
// Keep the wording in step with what the reader does; test/render.test.mjs
// checks that every link resolves within this folder.

export const TUTORIAL_FOLDER = 'MD Reader Tutorial';
export const TUTORIAL_START = `${TUTORIAL_FOLDER}/1 Start here.md`;

const startHere = `# Start here

Welcome to MD Reader. This short tutorial teaches the controls on your glasses. Hold your head level and read on.

## Turning pages

Look **down** a little and hold it for a moment: the next page appears. Then come back to level to read it.

Look **up** the same way to go back a page.

Each look turns one page. Look back to level before the next one.

## The tilt guide

The thin line on the right edge is the tilt guide. The dot shows where your head is. The two short ticks are how far you need to tilt to turn a page.

You can change that angle on the phone, under Head control, with **Tilt to page**. The ticks move with it.

## Which way did it go?

After every page turn, a thin arrow shows the direction: pointing down below the text, or up above it. It stays dim until the next turn.

## Straight ahead

The glasses remember where "level" is for you. If paging starts to feel off, look where you like to read and tap the glasses once, or press **Recentre tilt** in the phone app.

A single tap also pauses head scrolling. Tap again to turn it back on.

## Practice picking links

Underlined words are links. Turn your head **right** to put a box around the next link, and **left** for the previous one. Nod once, down and back up, to open the boxed link. Double-clicking the glasses works too.

Try moving the box between these two links, then open one:

- [[2 Links and going back|Links and going back]]
- [[3 Continuous scrolling|Continuous scrolling]]
`;

const linksAndBack = `# Links and going back

You opened a link. Practise moving the box through these with left and right head turns:

- [[1 Start here|Start here]], the first page
- [[1 Start here#Turning pages|Turning pages]], partway down
- [[3 Continuous scrolling|Continuous scrolling]]
- [[4 Display and brightness|Display and brightness]]

## Going back

To return to where you were, **long-press** the glasses. You come back to the same page, with the same link boxed.

## Good to know

- With a link boxed, a page turn waits about a second, so a nod is never taken as a look down.
- The phone app shows the same note. Tap any word there to jump the glasses to that spot.
- A ring works too: left and right pick links, up and down turn pages.

When you are done, long-press to go back, or open the next part: [[3 Continuous scrolling|Continuous scrolling]].
`;

const stretches = [
  'Hold your look down and pages keep coming, one per pause.',
  'Come back to level and the scrolling stops.',
  'Hold your look up to scroll back the same way.',
  'A longer pause gives you more time to read each page.',
  'The arrow below the text means you went forward.',
  'The arrow above the text means you went back.',
  'If the dot on the tilt guide stays past a tick, you keep scrolling.',
  'Tap the glasses if you want everything to stop for a while.',
  'Auto-scroll is the same idea, but you can look anywhere.',
  'Try a few different pause lengths and keep the one you like.',
  'Halfway there. Look up for a moment to scroll back a page or two.',
  'Then look down again to keep going.',
  'Nearly done. One or two more pages.',
];

const continuous = `# Continuous scrolling

This part is long on purpose, so you can practise scrolling through it.

## Turning it on

In the phone app, open Head control and turn on **Keep scrolling while I look up/down**. Then set **Pause between pages**, from 1 to 10 seconds.

Now, instead of one page per look, holding your look down keeps turning pages, one each pause. Holding it up scrolls back.

## Auto-scroll

The **Start auto-scroll** button on the phone turns pages on a timer, wherever you look, using the same pause. Tap the glasses to stop it. It also stops at the end of a note.

## Practice

${stretches.map((line, index) => `### Stretch ${index + 1}\n\n${line} This paragraph is here to give you something to read while the pages go by, at whatever pace feels right.`).join('\n\n')}

## The end

You reached the end. Look up to scroll back, or open [[4 Display and brightness|Display and brightness]]. You can also return to [[1 Start here|Start here]].
`;

const display = `# Display and brightness

## Turning the display off

Shake your head "no" a few times, quickly, and the display turns off. The status line counts the shakes as you go.

Nod a few times to turn it back on, or press any button on the glasses.

While the display is off, nothing scrolls.

## Brightness

The glasses' automatic brightness can dim the display as you look around. **Hold glasses brightness** on the phone keeps it steady. Pick a fixed level, or keep the current one.

## If something does not respond

Open **Glasses log** at the bottom of the phone app. It lists what the glasses notice, such as each shake or nod and how fast it was. If nods are not picked up, lower **Nod needed to open a link**.

## Finishing the tutorial

That's everything. When you no longer need these notes, tap **Hide tutorial** in the vault list on the phone. You can bring them back later from **Imported files**.

Back to [[1 Start here|Start here]].
`;

export const TUTORIAL_NOTES = Object.freeze({
  [TUTORIAL_START]: startHere,
  [`${TUTORIAL_FOLDER}/2 Links and going back.md`]: linksAndBack,
  [`${TUTORIAL_FOLDER}/3 Continuous scrolling.md`]: continuous,
  [`${TUTORIAL_FOLDER}/4 Display and brightness.md`]: display,
});
