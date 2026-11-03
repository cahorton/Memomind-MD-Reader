// Wire format shared with glass/md_reader/md_reader.c. Big-endian throughout.

export const MD_CHANNEL = 0x4d44;
export const MD_EVENT_CHANNEL = 0x4d45;
export const PROTOCOL_VERSION = 1;
export const NO_LINK = 0xffff;
export const GLASSES_WINDOW_BYTES = 8192;
export const MAX_WINDOW_LINKS = 128;
const MAX_TITLE_BYTES = 160;
const MAX_NOTICE_BYTES = 96;

export const settingFlags = Object.freeze({
  invertPitch: 0x01,
  invertYaw: 0x02,
  tiltGuide: 0x04,
  gestureSleep: 0x08,
  nodOpen: 0x10,
  invertedLink: 0x20,
});

const encoder = new TextEncoder();

export function utf8Prefix(bytes, maximum) {
  let length = Math.min(bytes.length, maximum);
  while (length > 0 && length < bytes.length && (bytes[length] & 0xc0) === 0x80) length -= 1;
  return bytes.subarray(0, length);
}

function header(length, command) {
  const data = new Uint8Array(length);
  data[0] = PROTOCOL_VERSION;
  data[1] = command;
  return { data, view: new DataView(data.buffer) };
}

export function encodeOpen({ session, docId, totalBytes, offset = 0, linkId = NO_LINK, historyDepth = 0, title = '' }) {
  const titleBytes = utf8Prefix(encoder.encode(String(title)), MAX_TITLE_BYTES);
  const { data, view } = header(21 + titleBytes.length, 1);
  view.setUint32(2, session);
  view.setUint32(6, docId);
  view.setUint32(10, totalBytes);
  view.setUint32(14, offset);
  view.setUint16(18, linkId);
  data[20] = Math.max(0, Math.min(255, historyDepth));
  data.set(titleBytes, 21);
  return data;
}

// links: [{ start, end, id }] in document byte offsets.
export function encodeWindow({ session, offset, final, links, bytes }) {
  if (links.length > MAX_WINDOW_LINKS) throw new RangeError('Too many links in one window');
  const { data, view } = header(13 + links.length * 10 + bytes.length, 2);
  view.setUint32(2, session);
  view.setUint32(6, offset);
  data[10] = final ? 1 : 0;
  view.setUint16(11, links.length);
  let cursor = 13;
  for (const link of links) {
    view.setUint32(cursor, link.start);
    view.setUint32(cursor + 4, link.end);
    view.setUint16(cursor + 8, link.id);
    cursor += 10;
  }
  data.set(bytes, cursor);
  return data;
}

export function encodeSettings(settings) {
  const { data, view } = header(16, 3);
  let flags = 0;
  if (settings.invertPitch) flags |= settingFlags.invertPitch;
  if (settings.invertYaw) flags |= settingFlags.invertYaw;
  if (settings.tiltGuide) flags |= settingFlags.tiltGuide;
  if (settings.gestureSleep) flags |= settingFlags.gestureSleep;
  if (settings.nodOpen) flags |= settingFlags.nodOpen;
  if (settings.linkHighlight === 'inverted') flags |= settingFlags.invertedLink;
  data[2] = settings.headEnabled ? 1 : 0;
  data[3] = Math.max(3, Math.min(45, Math.round(settings.pitchThreshold)));
  // The repeat interval is the continuous-mode pause; 0 means one page per look.
  const repeatMs = settings.continuous ? Math.round(settings.pauseMs) : 0;
  view.setUint16(4, repeatMs <= 0 ? 0 : Math.max(500, Math.min(10000, repeatMs)));
  view.setUint16(6, Math.max(0, Math.min(2000, Math.round(settings.dwellMs))));
  view.setUint16(8, Math.max(20, Math.min(2000, Math.round(settings.turnThreshold))));
  data[10] = flags;
  data[11] = settings.largeFont ? 1 : 0;
  // Brightness: hold blocks automatic changes; level 0 keeps the current level.
  data[12] = settings.lockBrightness ? 1 : 0;
  data[13] = Math.max(0, Math.min(10, Math.round(settings.brightnessLevel ?? 0)));
  // Nod speed needed to open a link (gentler than the link-turn threshold).
  view.setUint16(14, Math.max(15, Math.min(2000, Math.round(settings.nodThreshold ?? 70))));
  return data;
}

// Recentre: after delayMs the glasses take the current head pose as neutral.
export function encodeRecentre(delayMs) {
  const { data, view } = header(4, 7);
  view.setUint16(2, Math.max(0, Math.min(10000, Math.round(delayMs))));
  return data;
}

// Auto-scroll: a page every intervalMs regardless of head position.
export function encodeAutoScroll({ session, on, intervalMs }) {
  const { data, view } = header(9, 6);
  view.setUint32(2, session);
  data[6] = on ? 1 : 0;
  view.setUint16(7, Math.max(500, Math.min(10000, Math.round(intervalMs))));
  return data;
}

export function encodeClose(session) {
  const { data, view } = header(6, 4);
  view.setUint32(2, session);
  return data;
}

export function encodeNotice(text) {
  const bytes = utf8Prefix(encoder.encode(String(text)), MAX_NOTICE_BYTES);
  const { data } = header(2 + bytes.length, 5);
  data.set(bytes, 2);
  return data;
}

export function decodeEvent(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 2 || bytes[0] !== PROTOCOL_VERSION) {
    throw new Error('Invalid MD reader event');
  }
  if (bytes[1] === 7) return { type: 'diag', text: new TextDecoder().decode(bytes.subarray(2)) };
  if (bytes.length < 6) throw new Error('Invalid MD reader event');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const session = view.getUint32(2);
  switch (bytes[1]) {
  case 1:
    if (bytes.length !== 12) break;
    return { type: 'needWindow', session, offset: view.getUint32(6), maxBytes: view.getUint16(10) };
  case 2:
    if (bytes.length !== 20) break;
    return {
      type: 'page', session,
      pageStart: view.getUint32(6), pageEnd: view.getUint32(10),
      pageIndex: view.getUint16(14), pageCount: view.getUint16(16),
      linkId: view.getUint16(18),
    };
  case 3:
    if (bytes.length !== 12) break;
    return { type: 'follow', session, linkId: view.getUint16(6), pageStart: view.getUint32(8) };
  case 4:
    if (bytes.length !== 6) break;
    return { type: 'back', session };
  case 5:
    if (bytes.length !== 6 && bytes.length !== 7) break;
    // Glasses builds from v7 on append their version.
    return { type: 'hello', session, glassesVersion: bytes.length === 7 ? bytes[6] : null };
  case 6:
    if (bytes.length !== 7) break;
    return { type: 'autoscroll', session, on: bytes[6] !== 0 };

  default:
    break;
  }
  throw new Error('Unsupported MD reader event');
}

// Builds the reply to a glasses window request from a rendered note.
export function windowFor(rendered, bytes, offset, maxBytes = GLASSES_WINDOW_BYTES) {
  const start = Math.min(offset, bytes.length);
  const slice = utf8Prefix(bytes.subarray(start), Math.min(maxBytes, GLASSES_WINDOW_BYTES));
  const end = start + slice.length;
  const links = [];
  rendered.links.forEach((link, id) => {
    if (links.length < MAX_WINDOW_LINKS && link.end > start && link.start < end) {
      links.push({ start: link.start, end: link.end, id });
    }
  });
  return { offset: start, final: end >= bytes.length, links, bytes: slice };
}
