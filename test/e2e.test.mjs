// End-to-end: the real glasses C code (compiled natively by test/host_sim.c)
// talks to the real phone ReaderController over the real wire format.

import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { decodeEvent } from '../phone/md-reader/md-protocol.js';
import { ReaderController } from '../phone/md-reader/reader-controller.js';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const simPath = join(root, '.build', 'host_sim');
const sdkInclude = process.env.GM_SDK_INCLUDE ?? join(root, '..', 'plugin-open-platform', 'GlassSDK', 'include');

const BUTTON = { primary: 1, up: 2, down: 3, left: 4, right: 5, back: 10 };
const GESTURE = { nod: 1, shake: 4 };
const ACTION = { single: 1, double: 2, long: 3, veryLong: 4, release: 5, trigger: 6 };
const encoder = new TextEncoder();
const decoder = new TextDecoder();

function longNote() {
  const lines = ['# Long', '', 'Intro links to [[Short]].', ''];
  for (let section = 1; section <= 40; section += 1) {
    lines.push(`## Section ${section}`, '');
    for (let paragraph = 0; paragraph < 3; paragraph += 1) {
      lines.push(`Paragraph ${paragraph} of section ${section} talks about head-tracked reading on small displays and keeps going for a while so that it wraps across several lines.`);
    }
    if (section % 10 === 0) lines.push(`Jump to [[#Section ${section + 5}]] or back to [[Short]].`);
    lines.push('');
  }
  return lines.join('\n');
}

const notes = {
  'Short.md': '# Short\n\nGo to [[Other|the other note]] or [[Long#Section 33]].\nMissing: [[Nowhere]].\n',
  'Other.md': '# Other\n\nBack to [[Short]].\n',
  'Wrapped.md': '# Wrapped\n\nThis sentence runs on long enough to [[Other|the other note with a long name]] wraps.\n',
  'Long.md': longNote(),
};

class Sim {
  constructor() {
    this.process = spawn(simPath, [], { stdio: ['pipe', 'pipe', 'inherit'] });
    this.lines = createInterface({ input: this.process.stdout });
    this.waiters = [];
    this.buffer = [];
    this.lines.on('line', (line) => {
      this.buffer.push(line);
      if (line === 'OK') {
        const output = this.buffer;
        this.buffer = [];
        this.waiters.shift()?.(output);
      }
    });
  }

  next() {
    return new Promise((resolve) => this.waiters.push(resolve));
  }

  command(line) {
    const reply = this.next();
    this.process.stdin.write(`${line}\n`);
    return reply;
  }

  close() {
    this.process.stdin.end();
  }
}

async function harness({ startPitch = 0 } = {}) {
  const sim = new Sim();
  const outbox = [];
  const vault = { paths: Object.keys(notes), readNote: async (path) => notes[path] };
  const diagnostics = [];
  const controller = new ReaderController({
    vault,
    send: async (bytes) => { outbox.push(bytes); },
    onChange: (reason, detail) => { if (reason === 'diag') diagnostics.push(detail); },
  });
  const rejected = [];

  async function feed(output) {
    for (const line of output) {
      if (line === 'REJECTED') rejected.push(line);
      if (!line.startsWith('EVT ')) continue;
      await controller.handleEvent(decodeEvent(Uint8Array.from(Buffer.from(line.slice(4), 'hex'))));
    }
    while (outbox.length) {
      const bytes = outbox.shift();
      await feed(await sim.command(`MSG ${Buffer.from(bytes).toString('hex')}`));
    }
  }

  // Phone-initiated sends (controller.open, applySettings) wait in the outbox.
  const run = async (line) => {
    await feed([]);
    await feed(await sim.command(line));
  };
  const state = async () => {
    await feed([]);
    return JSON.parse((await sim.command('DUMP')).find((line) => line.startsWith('{')));
  };
  await feed(await sim.next());
  // Head tracking calibrates on the pose held at start.
  await run(`IMU ${startPitch} 0 0 0`);
  await run('TICK 400');
  return { sim, controller, run, state, rejected, diagnostics };
}

function selectedText(view) {
  return view.selectedText;
}

// Rendered text of the glasses page as reported by the phone controller.
function phonePage(controller) {
  const { pageStart, pageEnd } = controller.page;
  return decoder.decode(controller.current.bytes.subarray(pageStart, pageEnd)).replace(/\n$/u, '');
}

test.before(() => {
  mkdirSync(join(root, '.build'), { recursive: true });
  execFileSync('cc', [
    '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-g', '-D_FORTIFY_SOURCE=0',
    '-fsanitize=address,undefined', '-I', sdkInclude,
    '-o', simPath, join(root, 'test', 'host_sim.c'),
  ], { stdio: 'inherit' });
});

test('opens a note, highlights links by head turn, follows and goes back', async () => {
  const { sim, controller, run, state, rejected } = await harness();
  try {
    await controller.open('Short.md');
    let view = await state();
    assert.equal(view.title, 'Short');
    assert.match(view.page, /^# Short\n\nGo to the other note or Long > Section 33\.\nMissing: Nowhere\./u);
    assert.equal(view.links, 3);
    assert.equal(view.hint, '3 links');
    assert.equal(view.underlines.length, 3);
    assert.equal(view.guideHidden, 0);
    assert.equal(view.selectedText, '');
    assert.deepEqual(view.boxes, []);

    // Flick right: the first link. The return swing must not move it back.
    await run('IMU 0 200 0 0');
    await run('TICK 100');
    await run('IMU 0 -200 0 0');
    await run('TICK 100');
    view = await state();
    assert.equal(selectedText(view), 'the other note');
    await run('IMU 0 0 0 0');
    await run('TICK 400');
    await run('IMU 0 200 0 0');
    await run('TICK 100');
    await run('IMU 0 0 0 0');
    await run('TICK 400');
    assert.equal(selectedText(await state()), 'Long > Section 33');
    // Flick left goes back to the previous link.
    await run('IMU 0 -200 0 0');
    await run('TICK 100');
    await run('IMU 0 0 0 0');
    await run('TICK 400');
    assert.equal(selectedText(await state()), 'the other note');

    // Double-click follows the selected link.
    await run(`BTN ${BUTTON.primary} ${ACTION.double}`);
    view = await state();
    assert.equal(view.title, 'Other');
    assert.equal(controller.history.length, 1);
    assert.equal(view.hint, '1 link');

    // Long press goes back and restores the selection.
    await run(`BTN ${BUTTON.primary} ${ACTION.long}`);
    await run(`BTN ${BUTTON.primary} ${ACTION.veryLong}`);
    await run(`BTN ${BUTTON.primary} ${ACTION.release}`);
    view = await state();
    assert.equal(view.title, 'Short');
    assert.equal(view.exitActive, 0);
    assert.equal(selectedText(view), 'the other note');
    assert.equal(controller.history.length, 0);

    // A link to a missing note says so on the glasses.
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    assert.equal(selectedText(await state()), 'Nowhere');
    await run(`BTN ${BUTTON.primary} ${ACTION.double}`);
    view = await state();
    assert.equal(view.title, 'Short');
    assert.equal(view.hint, 'Not found: Nowhere');
    assert.deepEqual(rejected, []);
  } finally {
    sim.close();
  }
});

test('head tilt pages through a long note one window at a time', async () => {
  const { sim, controller, run, state, rejected } = await harness();
  try {
    await controller.open('Long.md');
    let view = await state();
    assert.equal(view.pageIndex, 0);
    assert.equal(view.page, phonePage(controller));

    // Below the threshold nothing moves.
    await run('IMU 8 0 0 0');
    await run('TICK 2000');
    assert.equal((await state()).pageIndex, 0);

    // A quick glance down shorter than the dwell does not page.
    await run('IMU -20 0 0 0');
    await run('TICK 200');
    await run('IMU 0 0 0 0');
    await run('TICK 200');
    assert.equal((await state()).pageIndex, 0);

    // Continuous mode: holding a look down pages once after the dwell, then
    // after every pause. The arrow below the text points down.
    await controller.applySettings({ continuous: true, pauseMs: 1500 });
    await run('IMU -20 0 0 0');
    await run('TICK 400');
    view = await state();
    assert.equal(view.pageIndex, 1);
    assert.equal(view.arrow, 1);
    assert.equal(view.arrowBright, 1);
    assert.ok(view.arrowTipY > view.viewportBottom, 'down arrow sits below the text');
    await run('TICK 1500');
    view = await state();
    assert.equal(view.pageIndex, 2);
    assert.equal(view.page, phonePage(controller));
    assert.equal(view.tiltZone, -1);

    // Back to centre, then looking up pages back with an up arrow on top.
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    await run('IMU 20 0 0 0');
    await run('TICK 400');
    view = await state();
    assert.equal(view.pageIndex, 1);
    assert.equal(view.page, phonePage(controller));
    assert.equal(view.arrow, -1);
    assert.ok(view.arrowTipY < view.viewportTop, 'up arrow sits above the text');
    await run('IMU 0 0 0 0');
    await run('TICK 2500');
    // The arrow dims but stays until the next page turn.
    view = await state();
    assert.equal(view.arrow, -1);
    assert.equal(view.arrowBright, 0);

    // Walk to the end with the ring: every page matches the phone text, and
    // pages tile the document with only blank lines between them.
    const text = controller.current.bytes;
    let previousEnd = controller.page.pageEnd;
    for (let guard = 0; guard < 500; guard += 1) {
      await run(`BTN ${BUTTON.down} ${ACTION.trigger}`);
      view = await state();
      if (view.hint === 'End of note') break;
      assert.equal(view.page, phonePage(controller));
      const gap = decoder.decode(text.subarray(previousEnd, controller.page.pageStart));
      assert.match(gap, /^\n*$/u);
      previousEnd = controller.page.pageEnd;
    }
    assert.equal(view.complete, 1);
    assert.equal(previousEnd, text.length);
    assert.equal(view.footer, `${view.pageCount} / ${view.pageCount}`);
    assert.ok(view.pageCount > 20, `expected many pages, got ${view.pageCount}`);
    assert.ok(text.length > 8192 * 2, 'the note must span several windows');

    // Back to the top with the ring, then one more press reports the top.
    for (let guard = 0; guard < 500 && (await state()).pageIndex > 0; guard += 1) {
      await run(`BTN ${BUTTON.up} ${ACTION.trigger}`);
    }
    await run(`BTN ${BUTTON.up} ${ACTION.trigger}`);
    view = await state();
    assert.equal(view.pageIndex, 0);
    assert.equal(view.hint, 'Top of note');
    assert.deepEqual(rejected, []);
  } finally {
    sim.close();
  }
});

test('heading links land on the right page, and font changes keep the place', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Short.md');
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    await run(`BTN ${BUTTON.primary} ${ACTION.double}`);
    let view = await state();
    assert.equal(view.title, 'Long');
    assert.ok(view.pageIndex > 5);
    assert.match(view.page, /## Section 33/u);

    // Same-note heading link reuses the layout and jumps forward.
    await controller.open('Long.md', { offset: 0 });
    for (let guard = 0; guard < 200 && !/Jump to Section 15/u.test((await state()).page); guard += 1) {
      await run(`BTN ${BUTTON.down} ${ACTION.trigger}`);
    }
    view = await state();
    const before = view.pageIndex;
    while (selectedText(view) !== 'Section 15') {
      await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
      view = await state();
    }
    await run(`BTN ${BUTTON.primary} ${ACTION.double}`);
    view = await state();
    assert.ok(view.pageIndex > before);
    assert.match(view.page, /## Section 15/u);

    // Large font: fewer lines per page, same reading position.
    const start = controller.page.pageStart;
    await controller.applySettings({ largeFont: true });
    await run('TICK 100');
    view = await state();
    assert.ok(view.linesPerPage < 9);
    assert.ok(controller.page.pageStart <= start && controller.page.pageEnd > start);
    assert.equal(view.page, phonePage(controller));
  } finally {
    sim.close();
  }
});

test('single click pauses head control; long press with no history exits', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Long.md');
    await run(`BTN ${BUTTON.primary} ${ACTION.single}`);
    let view = await state();
    assert.equal(view.headEnabled, 0);
    assert.equal(view.guideHidden, 1);
    await run('IMU 25 0 0 0');
    await run('TICK 3000');
    assert.equal((await state()).pageIndex, 0);

    // Re-enabling recentres on the current pose, so the tilt is now neutral.
    await run(`BTN ${BUTTON.primary} ${ACTION.single}`);
    await run('TICK 3000');
    view = await state();
    assert.equal(view.headEnabled, 1);
    assert.equal(view.pageIndex, 0);
    await run('IMU 5 0 0 0');
    await run('TICK 400');
    assert.equal((await state()).pageIndex, 1);

    await run(`BTN ${BUTTON.primary} ${ACTION.long}`);
    assert.equal((await state()).exitActive, 1);
    await run(`BTN ${BUTTON.primary} ${ACTION.release}`);
    assert.equal((await state()).exitActive, 0);
    await run(`BTN ${BUTTON.primary} ${ACTION.long}`);
    await run('TICK 3100');
    assert.equal((await state()).exited, 1);
  } finally {
    sim.close();
  }
});

test('calibrating while looking down at the phone settles instead of paging forever', async () => {
  const { sim, controller, run, state } = await harness({ startPitch: -35 });
  try {
    await controller.open('Long.md');
    // Looking straight ahead to read is 35 degrees "up" from that neutral.
    // Start a few pages in so a stray page back would show.
    await run(`BTN ${BUTTON.down} ${ACTION.trigger}`);
    await run(`BTN ${BUTTON.down} ${ACTION.trigger}`);
    await run(`BTN ${BUTTON.down} ${ACTION.trigger}`);
    await run('IMU 0 0 0 0');
    await run('TICK 15000');
    let view = await state();
    assert.ok(view.pageIndex >= 2, `paged back ${3 - view.pageIndex} times while reading straight ahead`);
    assert.equal(view.tiltZone, 0);
    const settled = view.pageIndex;

    // Straight ahead is now neutral: a real look down pages once.
    await run('IMU -20 0 0 0');
    await run('TICK 600');
    assert.equal((await state()).pageIndex, settled + 1);
    await run('IMU 0 0 0 0');
    await run('TICK 2000');
    assert.equal((await state()).pageIndex, settled + 1);
  } finally {
    sim.close();
  }
});

test('by default a held tilt turns one page, and coming back does not page back', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Long.md');
    await run('IMU -20 0 0 0');
    await run('TICK 5000');
    assert.equal((await state()).pageIndex, 1);
    await run('IMU 0 0 0 0');
    await run('TICK 2000');
    assert.equal((await state()).pageIndex, 1);
    // Next look down turns the next page.
    await run('IMU -20 0 0 0');
    await run('TICK 600');
    await run('IMU 0 0 0 0');
    await run('TICK 1500');
    assert.equal((await state()).pageIndex, 2);
    // And looking up goes back one.
    await run('IMU 20 0 0 0');
    await run('TICK 600');
    await run('IMU 0 0 0 0');
    await run('TICK 1500');
    assert.equal((await state()).pageIndex, 1);
  } finally {
    sim.close();
  }
});

test('phone settings reach the glasses and say so', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Long.md');
    assert.equal((await state()).pageIndex, 0);
    await controller.applySettings({ pitchThreshold: 25 });
    let view = await state();
    assert.equal(view.hint, 'Settings updated');
    // 20 degrees is now below the threshold.
    await run('IMU -20 0 0 0');
    await run('TICK 3000');
    view = await state();
    assert.equal(view.pageIndex, 0);
  } finally {
    sim.close();
  }
});

test('continuous mode keeps scrolling while the look is held, up to a 10 s pause', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Long.md');
    await controller.applySettings({ continuous: true, pauseMs: 10000 });
    // Confirm neutral the way a reader does: one look down and back.
    await run('IMU -20 0 0 0');
    await run('TICK 400');
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    assert.equal((await state()).pageIndex, 1);

    // Hold the look down for a minute: a page right away, then one per 10 s.
    await run('IMU -20 0 0 0');
    await run('TICK 400');
    assert.equal((await state()).pageIndex, 2);
    await run('TICK 9500');
    assert.equal((await state()).pageIndex, 2);
    await run('TICK 600');
    assert.equal((await state()).pageIndex, 3);
    await run('TICK 50000');
    assert.equal((await state()).pageIndex, 8);
  } finally {
    sim.close();
  }
});

test('auto-scroll pages on a timer wherever you look, and stops on a tap or at the end', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Long.md');
    await controller.applySettings({ pauseMs: 2000 });
    await controller.setAutoScroll(true);
    let view = await state();
    assert.equal(view.autoScroll, 1);
    assert.equal(view.hint, 'Auto-scroll on');
    assert.equal(view.guideHidden, 1);
    await run('TICK 2100');
    view = await state();
    assert.equal(view.pageIndex, 1);
    assert.equal(view.arrow, 1);
    assert.equal(view.hint, 'Auto-scroll  (tap to stop)');

    // Looking up or down does not change the pace.
    await run('IMU 25 0 0 0');
    await run('TICK 1000');
    assert.equal((await state()).pageIndex, 1);
    await run('IMU -25 0 0 0');
    await run('TICK 1000');
    assert.equal((await state()).pageIndex, 2);
    await run('IMU 0 0 0 0');

    // A new pause applies while it runs.
    await controller.applySettings({ pauseMs: 5000 });
    await run('TICK 4000');
    assert.equal((await state()).pageIndex, 2);
    await run('TICK 1200');
    assert.equal((await state()).pageIndex, 3);

    // A tap on the glasses stops it and the phone hears about it.
    await run(`BTN ${BUTTON.primary} ${ACTION.single}`);
    view = await state();
    assert.equal(view.autoScroll, 0);
    assert.equal(view.hint, 'Auto-scroll off');
    assert.equal(view.headEnabled, 1);
    assert.equal(controller.autoScroll, false);
    await run('TICK 12000');
    assert.equal((await state()).pageIndex, 3);

    // At the end of a note it stops by itself.
    await controller.open('Short.md');
    await controller.setAutoScroll(true);
    await run('TICK 5200');
    view = await state();
    assert.equal(view.autoScroll, 0);
    assert.equal(view.hint, 'End of note');
    assert.equal(controller.autoScroll, false);
  } finally {
    sim.close();
  }
});

test('brightness can be held, at the current level or a fixed one', async () => {
  const { sim, controller, state } = await harness();
  try {
    // The default holds the current level: auto-dimming blocked, level kept.
    let view = await state();
    assert.equal(view.brightnessBlocked, 1);
    assert.equal(view.brightness, 5);
    await controller.applySettings({ brightnessLevel: 8 });
    view = await state();
    assert.equal(view.brightnessBlocked, 1);
    assert.equal(view.brightness, 8);
    await controller.applySettings({ lockBrightness: false });
    assert.equal((await state()).brightnessBlocked, 0);
  } finally {
    sim.close();
  }
});

test('tilt guide ticks sit at the threshold: closer together at 6 than at 12 degrees', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Long.md');
    let view = await state();
    const gap12 = view.ticks[1] - view.ticks[0];
    assert.equal(view.middle - view.ticks[0], view.ticks[1] - view.middle);
    await controller.applySettings({ pitchThreshold: 6 });
    view = await state();
    const gap6 = view.ticks[1] - view.ticks[0];
    assert.ok(Math.abs(gap6 * 2 - gap12) <= 2, `6 degrees gap ${gap6} should be half of 12 degrees gap ${gap12}`);
    // The marker reaches a tick exactly when the tilt reaches the threshold.
    await run('IMU 6 0 0 0');
    await run('TICK 100');
    view = await state();
    assert.ok(Math.abs(view.marker - view.ticks[0]) <= 1, `marker ${view.marker} vs tick ${view.ticks[0]}`);
  } finally {
    sim.close();
  }
});

test('three head shakes turn the display off; three nods turn it back on', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Long.md');
    assert.equal((await state()).imuMode, 3);
    // Shakes spread too far apart do nothing.
    await run(`GESTURE ${GESTURE.shake}`);
    await run('TICK 2500');
    await run(`GESTURE ${GESTURE.shake}`);
    await run('TICK 2500');
    await run(`GESTURE ${GESTURE.shake}`);
    assert.equal((await state()).screenOn, 1);
    // Three within a few seconds: off. Gestures and raw samples stay on so a
    // nod can wake it (IMU mode bits: 1 gestures, 2 raw).
    await run('TICK 600');
    await run(`GESTURE ${GESTURE.shake}`);
    await run('TICK 600');
    await run(`GESTURE ${GESTURE.shake}`);
    let view = await state();
    assert.equal(view.screenOn, 0);
    assert.equal(view.imuMode, 3);

    // While off, tilting and auto-scroll do not page.
    await controller.setAutoScroll(true);
    await run('IMU -25 0 0 0');
    await run('TICK 10000');
    assert.equal((await state()).pageIndex, 0);
    await controller.setAutoScroll(false);
    await run('IMU 0 0 0 0');

    // Two nods are not enough; the third wakes it and head tracking resumes.
    await run(`GESTURE ${GESTURE.nod}`);
    await run('TICK 500');
    await run(`GESTURE ${GESTURE.nod}`);
    assert.equal((await state()).screenOn, 0);
    await run('TICK 500');
    await run(`GESTURE ${GESTURE.nod}`);
    view = await state();
    assert.equal(view.screenOn, 1);
    assert.equal(view.imuMode, 3);
    assert.equal(view.hint, 'Display on');

    // Any button also wakes it, and that press does nothing else.
    for (let index = 0; index < 3; index += 1) {
      await run(`GESTURE ${GESTURE.shake}`);
      await run('TICK 300');
    }
    assert.equal((await state()).screenOn, 0);
    await run(`BTN ${BUTTON.down} ${ACTION.trigger}`);
    view = await state();
    assert.equal(view.screenOn, 1);
    assert.equal(view.pageIndex, 0);

    // Turned off in settings, shakes are ignored and gestures are not enabled.
    await controller.applySettings({ gestureSleep: false });
    assert.equal((await state()).imuMode, 2);
    for (let index = 0; index < 3; index += 1) {
      await run(`GESTURE ${GESTURE.shake}`);
      await run('TICK 300');
    }
    assert.equal((await state()).screenOn, 1);
  } finally {
    sim.close();
  }
});

test('head shakes and nods are also read from the gyro, with a log on the phone', async () => {
  const { sim, controller, run, state, diagnostics } = await harness();
  try {
    await controller.open('Long.md');
    assert.ok(diagnostics.some((line) => /glasses plugin v\d+ started: caps=0x[0-9a-f]+ raw=1 gestures=1 display=1/u.test(line)),
      diagnostics.join('\n'));
    assert.ok(diagnostics.some((line) => line.startsWith('settings: head=1 tilt=12')));

    // A link flick and its return is not a shake.
    await run('IMU 0 200 0 0');
    await run('TICK 100');
    await run('IMU 0 -200 0 0');
    await run('TICK 100');
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    let view = await state();
    assert.equal(view.screenOn, 1);
    assert.notEqual(view.hint.slice(0, 5), 'Shake');

    // Shaking "no": left, right, left, right, left.
    const hints = [];
    for (let swing = 0; swing < 5; swing += 1) {
      await run(`IMU 0 ${swing % 2 ? -250 : 250} 0 0`);
      await run('TICK 120');
      hints.push((await state()).hint);
    }
    view = await state();
    assert.equal(view.screenOn, 0);
    assert.ok(hints.includes('Shake 3/5') && hints.includes('Shake 4/5'), hints.join(', '));
    assert.ok(diagnostics.includes('shake detected from gyro'));
    assert.ok(diagnostics.includes('screen_turn_on(0) -> 0'));

    // Nodding: down, up, down, up, down.
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    for (let swing = 0; swing < 5; swing += 1) {
      await run(`IMU 0 0 0 ${swing % 2 ? 250 : -250}`);
      await run('TICK 120');
    }
    view = await state();
    assert.equal(view.screenOn, 1);
    assert.equal(view.hint, 'Display on');
    assert.ok(diagnostics.includes('screen_turn_on(1) -> 0'));

    // Swings too far apart do not add up.
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    for (let swing = 0; swing < 6; swing += 1) {
      await run(`IMU 0 ${swing % 2 ? -250 : 250} 0 0`);
      await run('TICK 80');
      await run('IMU 0 0 0 0');
      await run('TICK 900');
    }
    assert.equal((await state()).screenOn, 1);
    // Firmware gestures are logged too.
    await run('GESTURE 4');
    assert.ok(diagnostics.includes('firmware gesture 4 active=1'));
  } finally {
    sim.close();
  }
});

test('the highlighted link gets a drawn box (or inverted text) around each of its lines', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Short.md');
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    let view = await state();
    assert.equal(view.selectedText, 'the other note');
    // "Go to " is 6 characters of 11 px on line 2 (29 px lines, 24 px font).
    assert.deepEqual(view.boxes, [{ x: 66 - 3, y: 2 * 29 - 2, w: 14 * 11 + 6, h: 24 + 4, text: '' }]);

    await controller.applySettings({ linkHighlight: 'inverted' });
    view = await state();
    assert.deepEqual(view.boxes.map((box) => box.text), ['the other note']);

    // A link wrapped over two lines gets two boxes.
    await controller.open('Wrapped.md');
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    view = await state();
    assert.equal(view.boxes.length, 2);
    assert.equal(view.boxes.map((box) => box.text).join(' '), view.selectedText.replace(/\s+/gu, ' ').trim());
    // Back to a one-line link: one box, and no leftover from the wrapped one.
    await controller.open('Short.md');
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    assert.equal((await state()).boxes.length, 1);
  } finally {
    sim.close();
  }
});

test('two nods open the highlighted link; a nod never turns the page', async () => {
  const { sim, controller, run, state } = await harness();
  try {
    await controller.open('Short.md');
    // Without a highlighted link, nodding does nothing.
    for (let swing = 0; swing < 4; swing += 1) {
      await run(`IMU 0 0 0 ${swing % 2 ? 250 : -250}`);
      await run('TICK 120');
    }
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    assert.equal((await state()).title, 'Short');

    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    const hints = [];
    for (let swing = 0; swing < 4; swing += 1) {
      await run(`IMU 0 0 0 ${swing % 2 ? 250 : -250}`);
      await run('TICK 120');
      hints.push((await state()).hint);
    }
    assert.ok(hints.includes('Nod again to open'), hints.join(', '));
    let view = await state();
    assert.equal(view.title, 'Other');
    assert.equal(controller.history.length, 1);

    // A slow nod with a real pitch dip, while a link is highlighted on a long
    // page: no page turn, and the link still opens.
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    await controller.open('Long.md');
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    assert.equal((await state()).selectedText, 'Short');
    for (let nod = 0; nod < 2; nod += 1) {
      await run('IMU -20 0 0 -250');
      await run('TICK 400');
      await run('IMU 0 0 0 250');
      await run('TICK 200');
    }
    view = await state();
    assert.equal(view.title, 'Short');

    // Turned off in settings, nods are ignored.
    await run('IMU 0 0 0 0');
    await run('TICK 1000');
    await controller.applySettings({ nodOpen: false });
    await run(`BTN ${BUTTON.right} ${ACTION.trigger}`);
    for (let swing = 0; swing < 4; swing += 1) {
      await run(`IMU 0 0 0 ${swing % 2 ? 250 : -250}`);
      await run('TICK 120');
    }
    assert.equal((await state()).title, 'Short');
  } finally {
    sim.close();
  }
});
