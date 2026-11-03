import { createGMPlugin } from './vendor/gm-plugin-web-sdk.esm.js';
import { MD_CHANNEL, MD_EVENT_CHANNEL, decodeEvent } from './md-protocol.js';
import { ReaderController, defaultSettings } from './reader-controller.js';
import { Vault } from './vault.js';
import { utf8Length } from './md-render.js';
import { PHONE_VERSION } from './version.js';
import { TUTORIAL_FOLDER, TUTORIAL_START } from './tutorial.js';
import { browseRoot, compareNames, dirname, listFolder, noteTitle } from './vault-links.js';

const SETTINGS_KEY = 'md-reader.settings.v1';
// Version 3 replaced repeatMs with continuous + pauseMs.
const SETTINGS_VERSION = 3;
const STATE_KEY = 'md-reader.state.v1';
const STATE_SAVE_DELAY_MS = 800;
const FOLDER_KEY = 'md-reader.folder.v1';
const UI_KEY = 'md-reader.ui.v1';
const TUTORIAL_KEY = 'md-reader.tutorial.v1';
const RECENTRE_DELAY_MS = 2000;

const $ = (selector) => document.querySelector(selector);
const bridgeState = $('#bridge-state');
const importMd = $('#import-md');
const importZip = $('#import-zip');
const search = $('#search');
const noteList = $('#note-list');
const folderPath = $('#folder-path');
const noteCount = $('#note-count');
const sourceList = $('#source-list');
const emptyState = $('#empty-state');
const readerView = $('#reader');
const backButton = $('#back');
const autoScrollButton = $('#auto-scroll');
const autoScrollInfo = $('#auto-scroll-info');
const previewPanel = $('#preview-panel');
// The note text follows the glasses' page only when you are not using it.
const FOLLOW_PAUSE_MS = 8000;
let lastPreviewTouch = 0;
let followedPageStart = -1;
const noteHeading = $('#note-title');
const breadcrumbs = $('#breadcrumbs');
const pageInfo = $('#page-info');
const preview = $('#preview');
const status = $('#status');
const versions = $('#versions');
let glassesVersion = null;

function showVersions() {
  versions.textContent = `v${PHONE_VERSION}${glassesVersion === null ? '' : ` · glasses v${glassesVersion}`}`;
}
const logLines = $('#log-lines');
const MAX_LOG_LINES = 300;
const logEntries = [];

const settingInputs = {
  headEnabled: $('#head-enabled'),
  tiltGuide: $('#tilt-guide'),
  pitchThreshold: $('#pitch-threshold'),
  dwellMs: $('#dwell'),
  continuous: $('#continuous'),
  pauseMs: $('#pause'),
  turnThreshold: $('#turn'),
  invertPitch: $('#invert-pitch'),
  invertYaw: $('#invert-yaw'),
  largeFont: $('#large-font'),
  gestureSleep: $('#gesture-sleep'),
  nodOpen: $('#nod-open'),
  nodThreshold: $('#nod'),
  linkHighlight: $('#link-highlight'),
  lockBrightness: $('#lock-brightness'),
  brightnessLevel: $('#brightness'),
};
const settingOutputs = {
  pitchThreshold: [$('#pitch-threshold-value'), (value) => `${value}°`],
  dwellMs: [$('#dwell-value'), (value) => `${(value / 1000).toFixed(2)} s`],
  pauseMs: [$('#pause-value'), (value) => `${(value / 1000).toFixed(1)} s`],
  brightnessLevel: [$('#brightness-value'), (value) => (value === 0 ? 'keep current' : `${value} of 10`)],
  turnThreshold: [$('#turn-value'), (value) => (value < 100 ? 'light' : value < 200 ? 'medium' : 'firm') + ` (${value})`],
  nodThreshold: [$('#nod-value'), (value) => (value < 60 ? 'light' : value < 110 ? 'medium' : 'firm') + ` (${value})`],
};

let gm;
let vault;
let controller;
let connected = false;
let eventQueue = Promise.resolve();
let saveTimer;
// The folder being browsed in the library ('' is the vault root).
let currentFolder = '';

function setStatus(message, isError = false) {
  status.textContent = message;
  status.classList.toggle('error', isError);
}

function log(source, text) {
  const time = new Date().toTimeString().slice(0, 8);
  logEntries.push(`${time} ${source} ${text}`);
  if (logEntries.length > MAX_LOG_LINES) logEntries.shift();
  logLines.textContent = logEntries.join('\n');
  logLines.scrollTop = logLines.scrollHeight;
}

async function copyLog() {
  const text = logEntries.join('\n');
  try {
    await navigator.clipboard.writeText(text);
    setStatus('Glasses log copied.');
  } catch {
    // Some WebViews block the clipboard: select the text for a manual copy.
    const range = document.createRange();
    range.selectNodeContents(logLines);
    const selection = getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    setStatus('Log selected. Use your phone\'s Copy to copy it.');
  }
}

function setConnection(text, isConnected) {
  bridgeState.textContent = text;
  bridgeState.classList.toggle('connected', isConnected);
}

async function send(bytes) {
  try {
    await gm.plugin.sendMessage(MD_CHANNEL, bytes);
  } catch (error) {
    setStatus(connected ? `Glasses did not receive the update: ${error.message}`
      : 'Glasses not connected. The note opens when they connect.', connected);
  }
}

// ---------------------------------------------------------------- library

function noteButton(path, showFolder) {
  const button = document.createElement('button');
  const title = document.createElement('strong');
  button.type = 'button';
  button.className = `note-card${path === controller.current?.path ? ' active' : ''}`;
  title.textContent = noteTitle(path);
  button.append(title);
  if (showFolder) {
    const folder = document.createElement('small');
    folder.textContent = dirname(path) || ' ';
    button.append(folder);
  }
  button.addEventListener('click', () => void openNote(path));
  return button;
}

function enterFolder(path) {
  currentFolder = path;
  void gm.storage.set(FOLDER_KEY, path).catch(() => undefined);
  renderLibrary();
  noteList.scrollTop = 0;
}

function renderFolderPath(root) {
  const crumbs = [];
  const parts = currentFolder.slice(root ? root.length + 1 : 0).split('/').filter(Boolean);
  const rootButton = document.createElement('button');
  rootButton.type = 'button';
  rootButton.textContent = root ? root.slice(root.lastIndexOf('/') + 1) : 'Vault';
  rootButton.disabled = parts.length === 0;
  rootButton.addEventListener('click', () => enterFolder(root));
  crumbs.push(rootButton);
  let path = root;
  parts.forEach((part, index) => {
    path = path ? `${path}/${part}` : part;
    const target = path;
    const separator = document.createElement('span');
    separator.textContent = '›';
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = part;
    button.disabled = index === parts.length - 1;
    button.addEventListener('click', () => enterFolder(target));
    crumbs.push(separator, button);
  });
  folderPath.replaceChildren(...crumbs);
}

function renderLibrary() {
  const filter = search.value.trim().toLowerCase();
  const paths = vault.paths;
  noteCount.textContent = `${paths.length} ${paths.length === 1 ? 'note' : 'notes'}`;
  if (filter) {
    // Searching looks through every folder.
    folderPath.hidden = true;
    noteList.replaceChildren(...paths
      .filter((path) => path.toLowerCase().includes(filter))
      .sort((left, right) => compareNames(noteTitle(left), noteTitle(right)))
      .map((path) => noteButton(path, true)));
  } else {
    const root = browseRoot(paths);
    // Fall back to the root if the folder is gone (removed or re-imported).
    const exists = paths.some((path) => path.startsWith(`${currentFolder}/`));
    const inside = !root || currentFolder.startsWith(`${root}/`);
    if (currentFolder !== root && !(exists && inside)) currentFolder = root;
    folderPath.hidden = false;
    renderFolderPath(root);
    const level = listFolder(paths, currentFolder);
    const rows = [];
    if (currentFolder !== root) {
      const up = document.createElement('button');
      up.type = 'button';
      up.className = 'up-card';
      up.textContent = '‹ Up';
      up.addEventListener('click', () => enterFolder(dirname(currentFolder)));
      rows.push(up);
    }
    for (const folder of level.folders) {
      const button = document.createElement('button');
      const icon = document.createElement('span');
      const name = document.createElement('strong');
      const count = document.createElement('small');
      button.type = 'button';
      button.className = 'folder-card';
      icon.className = 'icon';
      icon.textContent = '▸';
      name.textContent = folder.name;
      count.textContent = `${folder.count} ${folder.count === 1 ? 'note' : 'notes'}`;
      button.append(icon, name, count);
      button.addEventListener('click', () => enterFolder(folder.path));
      rows.push(button);
    }
    rows.push(...level.notes.map((path) => noteButton(path, false)));
    noteList.replaceChildren(...rows);
  }
  sourceList.replaceChildren(...vault.sources.map((source) => {
    const row = document.createElement('div');
    const label = document.createElement('span');
    const remove = document.createElement('button');
    row.className = 'source-row';
    label.textContent = source.error
      ? `${source.name} · ${source.error}`
      : `${source.name} · ${source.notes} ${source.notes === 1 ? 'note' : 'notes'}`;
    remove.type = 'button';
    remove.className = 'danger';
    remove.textContent = 'Remove';
    remove.addEventListener('click', () => void removeSource(source));
    row.append(label, remove);
    return row;
  }));
  if (vault.sources.length === 0) sourceList.textContent = 'Nothing imported yet.';
  $('#hide-tutorial').hidden = vault.tutorialHidden;
  $('#show-tutorial').hidden = !vault.tutorialHidden;
}

async function reloadVault() {
  await vault.reload();
  controller.forgetDocuments();
  renderLibrary();
}

async function importNotes(kind) {
  try {
    setStatus('Opening the file picker…');
    const { count, replaced } = await vault.pick(kind);
    if (count === 0) {
      setStatus('Import cancelled.');
      return;
    }
    await reloadVault();
    // Re-render the open note in case its text just changed.
    if (controller.current && vault.notes.has(controller.current.path)) await controller.reopen();
    setStatus(`Imported ${count} ${count === 1 ? 'file' : 'files'}${replaced ? ` (replacing ${replaced} older ${replaced === 1 ? 'copy' : 'copies'})` : ''}. The vault has ${vault.paths.length} notes.`);
  } catch (error) {
    setStatus(`Import failed: ${error.message}`, true);
  }
}

async function removeSource(source) {
  // Import copied the file into MD Reader's own storage; this deletes only
  // that copy. The original in Files (or wherever it came from) stays.
  if (!confirm(`Remove ${source.name} from MD Reader?\n\nYour original file is not touched.`)) return;
  try {
    await vault.removeSource(source.fileId);
    await reloadVault();
    if (controller.current && !vault.notes.has(controller.current.path)) {
      controller.current = null;
      controller.page = null;
      renderReader();
    }
    setStatus(`Removed ${source.name} from MD Reader. The original file is unchanged.`);
  } catch (error) {
    setStatus(`Could not remove ${source.name}: ${error.message}`, true);
  }
}

async function setTutorialHidden(hidden) {
  vault.tutorialHidden = hidden;
  await gm.storage.set(TUTORIAL_KEY, { hidden }).catch(() => undefined);
  await reloadVault();
  if (hidden) {
    // Drop the tutorial from history, and close it if it is open.
    controller.history = controller.history.filter((entry) => !entry.path.startsWith(`${TUTORIAL_FOLDER}/`));
    if (controller.current?.path.startsWith(`${TUTORIAL_FOLDER}/`)) {
      controller.current = null;
      controller.page = null;
      renderReader();
    }
    setStatus('Tutorial hidden. Bring it back from Imported files.');
  } else {
    await openNote(TUTORIAL_START);
  }
}

async function openNote(path) {
  try {
    await controller.open(path, { push: Boolean(controller.current) });
  } catch (error) {
    setStatus(`Could not open ${noteTitle(path)}: ${error.message}`, true);
  }
}

// ---------------------------------------------------------------- reader

// Splits rendered text at the given UTF-8 byte offsets.
function sliceByBytes(text, cuts) {
  const marks = new Set(cuts);
  const pieces = [];
  let bytes = 0;
  let from = 0;
  let fromBytes = 0;
  for (let index = 0; index < text.length;) {
    if (marks.has(bytes) && index > from) {
      pieces.push({ text: text.slice(from, index), start: fromBytes, end: bytes });
      from = index;
      fromBytes = bytes;
    }
    const code = text.charCodeAt(index);
    const width = code < 0x80 ? 1 : code < 0x800 ? 2 : code >= 0xd800 && code <= 0xdbff ? 4 : 3;
    index += width === 4 ? 2 : 1;
    bytes += width;
  }
  if (from < text.length) pieces.push({ text: text.slice(from), start: fromBytes, end: bytes });
  return pieces;
}

function renderPreview() {
  // Collapsed: nothing to draw until it is opened again.
  if (!previewPanel.open) return;
  const doc = controller.current;
  const page = controller.page;
  const { links } = doc.rendered;
  const pageStart = page?.pageStart ?? 0;
  const pageEnd = page?.pageEnd || pageStart;
  const cuts = [pageStart, pageEnd, ...links.flatMap((link) => [link.start, link.end])];
  // Page breaks (form feeds) get their own piece, drawn as a dashed rule.
  for (let index = doc.rendered.text.indexOf('\f'); index >= 0; index = doc.rendered.text.indexOf('\f', index + 1)) {
    const byte = utf8Length(doc.rendered.text.slice(0, index));
    // The rule is a block; the newline after the break would add a gap.
    cuts.push(byte, byte + (doc.rendered.text[index + 1] === '\n' ? 2 : 1));
  }
  const nodes = [];
  let linkIndex = 0;
  let button = null;
  let buttonId = -1;
  let firstOnGlasses = null;
  for (const piece of sliceByBytes(doc.rendered.text, cuts)) {
    while (linkIndex < links.length && links[linkIndex].end <= piece.start) linkIndex += 1;
    const id = linkIndex < links.length && links[linkIndex].start <= piece.start ? linkIndex : -1;
    const span = document.createElement('span');
    const isBreak = piece.text === '\f' || piece.text === '\f\n';
    span.textContent = isBreak ? '' : piece.text;
    span.dataset.start = String(piece.start);
    if (isBreak) span.classList.add('page-break');
    if (pageEnd > pageStart && piece.start >= pageStart && piece.end <= pageEnd) {
      span.classList.add('on-glasses');
      firstOnGlasses ??= span;
    }
    if (id < 0) {
      nodes.push(span);
      continue;
    }
    if (id !== buttonId) {
      button = document.createElement('button');
      button.type = 'button';
      button.className = `link${id === page?.linkId ? ' selected' : ''}`;
      button.title = links[id].target.note || `#${links[id].target.heading}`;
      button.addEventListener('click', () => void controller.follow(id));
      buttonId = id;
      nodes.push(button);
    }
    button.append(span);
  }
  preview.replaceChildren(...nodes);
  followGlasses(firstOnGlasses, pageStart);
}

// Keeps the glasses' page in view inside the note box (never scrolling the
// whole phone page), once per page turn, and not while you are scrolling.
function followGlasses(span, pageStart) {
  if (!span || pageStart === followedPageStart) return;
  if (Date.now() - lastPreviewTouch < FOLLOW_PAUSE_MS) return;
  followedPageStart = pageStart;
  const top = span.offsetTop;
  if (top < preview.scrollTop || top > preview.scrollTop + preview.clientHeight - 40) {
    preview.scrollTop = Math.max(0, top - 12);
  }
}

function caretAt(x, y) {
  if (document.caretPositionFromPoint) {
    const position = document.caretPositionFromPoint(x, y);
    return position && { node: position.offsetNode, offset: position.offset };
  }
  if (document.caretRangeFromPoint) {
    const range = document.caretRangeFromPoint(x, y);
    return range && { node: range.startContainer, offset: range.startOffset };
  }
  return null;
}

// Byte offset in the rendered note of the character at a tap.
function offsetAtTap(event) {
  const caret = caretAt(event.clientX, event.clientY);
  const span = caret?.node?.nodeType === Node.TEXT_NODE ? caret.node.parentElement : null;
  if (span?.dataset.start !== undefined) {
    return Number(span.dataset.start) + utf8Length(caret.node.data.slice(0, caret.offset));
  }
  const piece = event.target.closest?.('[data-start]');
  return piece ? Number(piece.dataset.start) : null;
}

function renderReader() {
  const doc = controller.current;
  emptyState.hidden = Boolean(doc);
  readerView.hidden = !doc;
  renderLibrary();
  if (!doc) return;
  const page = controller.page;
  noteHeading.textContent = doc.title;
  backButton.disabled = controller.history.length === 0;
  breadcrumbs.textContent = controller.history.slice(-3).map((entry) => noteTitle(entry.path)).join(' › ');
  autoScrollButton.textContent = controller.autoScroll ? 'Stop auto-scroll' : 'Start auto-scroll';
  autoScrollInfo.textContent = controller.autoScroll
    ? `A page every ${(controller.settings.pauseMs / 1000).toFixed(1)} s. Tap the glasses to stop.`
    : `Pages every ${(controller.settings.pauseMs / 1000).toFixed(1)} s, wherever you look.`;
  pageInfo.textContent = !page?.pageEnd ? ''
    : page.pageCount ? `Page ${page.pageIndex + 1} of ${page.pageCount}`
      : `Page ${page.pageIndex + 1}`;
  renderPreview();
}

function scheduleSave() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    void gm.storage.set(STATE_KEY, controller.snapshot()).catch(() => undefined);
  }, STATE_SAVE_DELAY_MS);
}

function onControllerChange(reason, detail) {
  if (reason === 'diag') {
    log('glasses', detail);
    return;
  }
  if (reason === 'missing') {
    setStatus(`No note named "${detail.target.note}" in the vault.`, true);
    return;
  }
  if (reason === 'open') {
    setStatus(`Showing ${controller.current.title} on the glasses.`);
    followedPageStart = -1;
  }
  renderReader();
  scheduleSave();
}

// ---------------------------------------------------------------- settings

function showSettings(settings) {
  for (const [name, input] of Object.entries(settingInputs)) {
    if (input.type === 'checkbox') input.checked = Boolean(settings[name]);
    else input.value = String(settings[name]);
  }
  for (const [name, [output, format]] of Object.entries(settingOutputs)) {
    output.textContent = format(Number(settings[name]));
  }
  $('#brightness-setting').hidden = !settings.lockBrightness;
}

function readSettings() {
  const settings = {};
  for (const [name, input] of Object.entries(settingInputs)) {
    settings[name] = input.type === 'checkbox' ? input.checked
      : input.tagName === 'SELECT' ? input.value : Number(input.value);
  }
  settings.settingsVersion = SETTINGS_VERSION;
  return settings;
}

async function saveSettings() {
  const settings = readSettings();
  showSettings(settings);
  await controller.applySettings(settings);
  log('phone', `sent settings: tilt ${settings.pitchThreshold}°, turn ${settings.turnThreshold}, shake-to-sleep ${settings.gestureSleep ? 'on' : 'off'}`);
  await gm.storage.set(SETTINGS_KEY, settings).catch(() => undefined);
  if (controller.current) renderReader();
}

// ---------------------------------------------------------------- startup

function handleMessage(message) {
  if (message.channel !== MD_EVENT_CHANNEL) return;
  let event;
  try {
    event = decodeEvent(message.data);
  } catch {
    log('phone', `unreadable glasses message (${message.data?.length ?? 0} bytes)`);
    return;
  }
  if (event.type === 'hello') {
    if (event.glassesVersion !== null) glassesVersion = event.glassesVersion;
    showVersions();
    log('phone', `glasses plugin started or reconnected${event.glassesVersion === null ? '' : ` (v${event.glassesVersion})`}`);
  }
  // Keep glasses events in order; a window reply must not overtake an open.
  eventQueue = eventQueue
    .then(() => controller.handleEvent(event))
    .catch((error) => setStatus(`Glasses request failed: ${error.message}`, true));
}

async function withTimeout(promise, ms, message) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(message)), ms);
    })]);
  } finally {
    clearTimeout(timer);
  }
}

async function initialize() {
  try {
    gm = createGMPlugin();
    await withTimeout(gm.ready(), 8000, 'Bridge start timed out');
  } catch (error) {
    setConnection('Bridge unavailable · Retry', false);
    setStatus(`Glasses Bridge unavailable: ${error.message}. Tap the top-right button to retry.`, true);
    return;
  }
  vault = new Vault(gm);
  controller = new ReaderController({ vault, send, onChange: onControllerChange });
  gm.plugin.onMessage(handleMessage);
  gm.device.onConnection((event) => {
    connected = event.connected === true;
    log('phone', connected ? 'glasses connected' : 'glasses disconnected');
    setConnection(connected ? 'Glasses connected' : 'Glasses disconnected', connected);
    if (connected) {
      eventQueue = eventQueue.then(async () => {
        await controller.applySettings({});
        await controller.reopen();
      }).catch(() => undefined);
    }
  });
  await gm.device.subscribeEvents(['connection']).catch(() => undefined);
  const info = await gm.device.getInfo().catch(() => ({}));
  connected = info?.connected === true;
  setConnection(connected ? 'Glasses connected' : 'Bridge ready', connected);

  const savedSettings = { ...((await gm.storage.get(SETTINGS_KEY).catch(() => null))?.value ?? {}) };
  delete savedSettings.repeatMs;
  const settings = { ...defaultSettings, ...savedSettings };
  showSettings(settings);
  await controller.applySettings(settings);

  currentFolder = (await gm.storage.get(FOLDER_KEY).catch(() => null))?.value ?? '';
  vault.tutorialHidden = (await gm.storage.get(TUTORIAL_KEY).catch(() => null))?.value?.hidden === true;
  const ui = (await gm.storage.get(UI_KEY).catch(() => null))?.value;
  if (ui && typeof ui.previewOpen === 'boolean') previewPanel.open = ui.previewOpen;
  try {
    await reloadVault();
  } catch (error) {
    setStatus(`Could not read the vault: ${error.message}`, true);
  }
  importMd.disabled = false;
  importZip.disabled = false;
  const saved = (await gm.storage.get(STATE_KEY).catch(() => null))?.value;
  try {
    await controller.restore(saved);
  } catch (error) {
    setStatus(`Could not reopen the last note: ${error.message}`, true);
  }
  // First run: start with the tutorial on the glasses.
  if (!controller.current && !saved?.path && !vault.tutorialHidden) {
    await controller.open(TUTORIAL_START).catch(() => undefined);
  }
  renderReader();
  if (!controller.current) {
    setStatus(vault.paths.length ? 'Pick a note to show it on the glasses.'
      : 'Add .md files or a zipped Obsidian vault to start.');
  }
}

importMd.addEventListener('click', () => void importNotes('md'));
$('#hide-tutorial').addEventListener('click', () => void setTutorialHidden(true));
$('#show-tutorial').addEventListener('click', () => void setTutorialHidden(false));
importZip.addEventListener('click', () => void importNotes('zip'));
search.addEventListener('input', () => renderLibrary());
backButton.addEventListener('click', () => void controller.back());
$('#log-copy').addEventListener('click', () => void copyLog());
$('#log-clear').addEventListener('click', () => {
  logEntries.length = 0;
  logLines.textContent = '';
});
autoScrollButton.addEventListener('click', () => void controller.setAutoScroll(!controller.autoScroll));
$('#recentre').addEventListener('click', async () => {
  await controller.recentre(RECENTRE_DELAY_MS);
  setStatus(`Look where you read: the glasses recentre in ${RECENTRE_DELAY_MS / 1000} s.`);
  log('phone', 'sent recentre');
});
for (const type of ['pointerdown', 'touchstart', 'wheel']) {
  preview.addEventListener(type, () => { lastPreviewTouch = Date.now(); }, { passive: true });
}
preview.addEventListener('click', (event) => {
  // Links open; a tap anywhere else shows that spot on the glasses.
  if (event.target.closest('.link') || !controller?.current) return;
  if (getSelection()?.toString()) return;
  const offset = offsetAtTap(event);
  if (offset === null) return;
  log('phone', `jump to byte ${offset}`);
  void controller.jumpTo(offset).then(() => setStatus('Showing that spot on the glasses.'));
});
previewPanel.addEventListener('toggle', () => {
  if (previewPanel.open && controller?.current) renderPreview();
  void gm?.storage.set(UI_KEY, { previewOpen: previewPanel.open }).catch(() => undefined);
});
bridgeState.addEventListener('click', () => {
  if (!controller) globalThis.location.reload();
});
for (const input of Object.values(settingInputs)) {
  input.addEventListener('input', () => showSettings(readSettings()));
  input.addEventListener('change', () => void saveSettings());
}
window.addEventListener('pagehide', () => {
  clearTimeout(saveTimer);
  if (controller) void gm.storage.set(STATE_KEY, controller.snapshot()).catch(() => undefined);
});

showVersions();
void initialize();
