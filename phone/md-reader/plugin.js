import { createGMPlugin } from './vendor/gm-plugin-web-sdk.esm.js';
import { MD_CHANNEL, MD_EVENT_CHANNEL, decodeEvent } from './md-protocol.js';
import { ReaderController, defaultSettings } from './reader-controller.js';
import { Vault } from './vault.js';
import { dirname, noteTitle } from './vault-links.js';

const SETTINGS_KEY = 'md-reader.settings.v1';
const STATE_KEY = 'md-reader.state.v1';
const STATE_SAVE_DELAY_MS = 800;

const $ = (selector) => document.querySelector(selector);
const bridgeState = $('#bridge-state');
const importMd = $('#import-md');
const importZip = $('#import-zip');
const search = $('#search');
const noteList = $('#note-list');
const noteCount = $('#note-count');
const sourceList = $('#source-list');
const emptyState = $('#empty-state');
const readerView = $('#reader');
const backButton = $('#back');
const noteHeading = $('#note-title');
const breadcrumbs = $('#breadcrumbs');
const pageInfo = $('#page-info');
const preview = $('#preview');
const status = $('#status');

const settingInputs = {
  headEnabled: $('#head-enabled'),
  tiltGuide: $('#tilt-guide'),
  pitchThreshold: $('#pitch-threshold'),
  dwellMs: $('#dwell'),
  repeatMs: $('#repeat'),
  turnThreshold: $('#turn'),
  invertPitch: $('#invert-pitch'),
  invertYaw: $('#invert-yaw'),
  largeFont: $('#large-font'),
};
const settingOutputs = {
  pitchThreshold: [$('#pitch-threshold-value'), (value) => `${value}°`],
  dwellMs: [$('#dwell-value'), (value) => `${(value / 1000).toFixed(2)} s`],
  repeatMs: [$('#repeat-value'), (value) => `${(value / 1000).toFixed(1)} s`],
  turnThreshold: [$('#turn-value'), (value) => (value < 100 ? 'light' : value < 200 ? 'medium' : 'firm') + ` (${value})`],
};

let gm;
let vault;
let controller;
let connected = false;
let eventQueue = Promise.resolve();
let saveTimer;

function setStatus(message, isError = false) {
  status.textContent = message;
  status.classList.toggle('error', isError);
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

function renderLibrary() {
  const filter = search.value.trim().toLowerCase();
  const paths = vault.paths.sort((left, right) => left.localeCompare(right));
  noteCount.textContent = `${paths.length} ${paths.length === 1 ? 'note' : 'notes'}`;
  noteList.replaceChildren(...paths
    .filter((path) => !filter || path.toLowerCase().includes(filter))
    .map((path) => {
      const button = document.createElement('button');
      const title = document.createElement('strong');
      const folder = document.createElement('small');
      button.type = 'button';
      button.className = `note-card${path === controller.current?.path ? ' active' : ''}`;
      title.textContent = noteTitle(path);
      folder.textContent = dirname(path) || ' ';
      button.append(title, folder);
      button.addEventListener('click', () => void openNote(path));
      return button;
    }));
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
}

async function reloadVault() {
  await vault.reload();
  controller.forgetDocuments();
  renderLibrary();
}

async function importNotes(kind) {
  try {
    setStatus('Opening the file picker…');
    const count = await vault.pick(kind);
    if (count === 0) {
      setStatus('Import cancelled.');
      return;
    }
    await reloadVault();
    setStatus(`Imported ${count} ${count === 1 ? 'file' : 'files'}. The vault has ${vault.paths.length} notes.`);
  } catch (error) {
    setStatus(`Import failed: ${error.message}`, true);
  }
}

async function removeSource(source) {
  if (!confirm(`Remove ${source.name} from the phone?`)) return;
  try {
    await vault.removeSource(source.fileId);
    await reloadVault();
    if (controller.current && !vault.notes.has(controller.current.path)) {
      controller.current = null;
      controller.page = null;
      renderReader();
    }
    setStatus(`Removed ${source.name}.`);
  } catch (error) {
    setStatus(`Could not remove ${source.name}: ${error.message}`, true);
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
  const doc = controller.current;
  const page = controller.page;
  const { links } = doc.rendered;
  const pageStart = page?.pageStart ?? 0;
  const pageEnd = page?.pageEnd || pageStart;
  const cuts = [pageStart, pageEnd, ...links.flatMap((link) => [link.start, link.end])];
  const nodes = [];
  let linkIndex = 0;
  let button = null;
  let buttonId = -1;
  let firstOnGlasses = null;
  for (const piece of sliceByBytes(doc.rendered.text, cuts)) {
    while (linkIndex < links.length && links[linkIndex].end <= piece.start) linkIndex += 1;
    const id = linkIndex < links.length && links[linkIndex].start <= piece.start ? linkIndex : -1;
    const span = document.createElement('span');
    span.textContent = piece.text;
    if (pageEnd > pageStart && piece.start >= pageStart && piece.end <= pageEnd) {
      span.className = 'on-glasses';
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
  firstOnGlasses?.scrollIntoView({ block: 'nearest' });
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
  if (reason === 'missing') {
    setStatus(`No note named "${detail.target.note}" in the vault.`, true);
    return;
  }
  if (reason === 'open') setStatus(`Showing ${controller.current.title} on the glasses.`);
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
}

function readSettings() {
  const settings = {};
  for (const [name, input] of Object.entries(settingInputs)) {
    settings[name] = input.type === 'checkbox' ? input.checked : Number(input.value);
  }
  return settings;
}

async function saveSettings() {
  const settings = readSettings();
  showSettings(settings);
  await controller.applySettings(settings);
  await gm.storage.set(SETTINGS_KEY, settings).catch(() => undefined);
}

// ---------------------------------------------------------------- startup

function handleMessage(message) {
  if (message.channel !== MD_EVENT_CHANNEL) return;
  let event;
  try {
    event = decodeEvent(message.data);
  } catch {
    return;
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

  const savedSettings = (await gm.storage.get(SETTINGS_KEY).catch(() => null))?.value;
  const settings = { ...defaultSettings, ...(savedSettings ?? {}) };
  showSettings(settings);
  await controller.applySettings(settings);

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
  renderReader();
  if (!controller.current) {
    setStatus(vault.paths.length ? 'Pick a note to show it on the glasses.'
      : 'Add .md files or a zipped Obsidian vault to start.');
  }
}

importMd.addEventListener('click', () => void importNotes('md'));
importZip.addEventListener('click', () => void importNotes('zip'));
search.addEventListener('input', () => renderLibrary());
backButton.addEventListener('click', () => void controller.back());
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

void initialize();
