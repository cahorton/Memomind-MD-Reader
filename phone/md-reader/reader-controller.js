// Phone-side reading state: which note the glasses show, the back history,
// and the replies to glasses events. Free of DOM and Bridge code so it can be
// tested in Node with a fake vault and transport.

import { findHeadingOffset, renderMarkdown } from './md-render.js';
import {
  NO_LINK,
  encodeNotice,
  encodeOpen,
  encodeSettings,
  encodeWindow,
  windowFor,
} from './md-protocol.js';
import { noteTitle, resolveNotePath } from './vault-links.js';

export const MAX_HISTORY = 64;

export const defaultSettings = Object.freeze({
  headEnabled: true,
  pitchThreshold: 12,
  dwellMs: 300,
  repeatMs: 1500,
  turnThreshold: 120,
  invertPitch: false,
  invertYaw: false,
  tiltGuide: true,
  largeFont: false,
});

const encoder = new TextEncoder();
const randomU32 = () => (Math.floor(Math.random() * 0xfffffff0) + 1) >>> 0;

export class ReaderController {
  // vault: { paths: string[], readNote(path): Promise<string> }
  // send: (bytes: Uint8Array) => Promise<void>
  constructor({ vault, send, onChange = () => {} }) {
    this.vault = vault;
    this.send = send;
    this.onChange = onChange;
    this.settings = { ...defaultSettings };
    this.history = [];
    this.current = null;
    this.page = null;
    this.session = randomU32();
    this.nextDocId = randomU32();
    this.docs = new Map();
  }

  // Drops cached renders, e.g. after the vault changes.
  forgetDocuments() {
    this.docs.clear();
  }

  async document(path) {
    if (this.docs.has(path)) return this.docs.get(path);
    const rendered = renderMarkdown(await this.vault.readNote(path));
    this.nextDocId = (this.nextDocId + 1) >>> 0 || 1;
    const doc = { path, title: noteTitle(path), rendered, bytes: encoder.encode(rendered.text), docId: this.nextDocId };
    this.docs.set(path, doc);
    return doc;
  }

  async open(path, { offset = 0, linkId = NO_LINK, push = false } = {}) {
    const doc = await this.document(path);
    if (push && this.current) {
      this.history.push({
        path: this.current.path,
        offset: this.page?.pageStart ?? 0,
        linkId: this.page?.linkId ?? NO_LINK,
      });
      if (this.history.length > MAX_HISTORY) this.history.shift();
    }
    this.current = doc;
    this.page = { pageStart: Math.min(offset, doc.bytes.length), pageEnd: 0, pageIndex: 0, pageCount: 0, linkId };
    this.session = (this.session + 1) >>> 0 || 1;
    await this.send(encodeOpen({
      session: this.session,
      docId: doc.docId,
      totalBytes: doc.bytes.length,
      offset: this.page.pageStart,
      linkId,
      historyDepth: this.history.length,
      title: doc.title,
    }));
    this.onChange('open');
    return doc;
  }

  async reopen() {
    if (!this.current) return;
    await this.open(this.current.path, { offset: this.page?.pageStart ?? 0, linkId: this.page?.linkId ?? NO_LINK });
  }

  async applySettings(settings) {
    this.settings = { ...this.settings, ...settings };
    await this.send(encodeSettings(this.settings));
  }

  resolve(link) {
    if (!link.target.note) return this.current.path;
    return resolveNotePath(link.target.note, this.current.path, this.vault.paths);
  }

  async follow(linkId, pageStart = this.page?.pageStart ?? 0) {
    const link = this.current?.rendered.links[linkId];
    if (!link) return false;
    const path = this.resolve(link);
    if (!path) {
      await this.send(encodeNotice(`Not found: ${link.target.note}`));
      this.onChange('missing', link);
      return false;
    }
    if (this.page) {
      this.page.pageStart = pageStart;
      this.page.linkId = linkId;
    }
    const target = await this.document(path);
    const offset = link.target.heading ? findHeadingOffset(target.rendered, link.target.heading) : 0;
    await this.open(path, { offset, push: true });
    return true;
  }

  async back() {
    const entry = this.history.pop();
    if (!entry) {
      await this.send(encodeNotice('No earlier note'));
      return false;
    }
    await this.open(entry.path, { offset: entry.offset, linkId: entry.linkId });
    return true;
  }

  async handleEvent(event) {
    if (event.type === 'hello') {
      await this.applySettings({});
      await this.reopen();
      return;
    }
    if (event.session !== this.session || !this.current) return;
    switch (event.type) {
    case 'needWindow': {
      const window = windowFor(this.current.rendered, this.current.bytes, event.offset, event.maxBytes);
      await this.send(encodeWindow({ session: this.session, ...window }));
      break;
    }
    case 'page':
      this.page = { ...event };
      this.onChange('page');
      break;
    case 'follow':
      await this.follow(event.linkId, event.pageStart);
      break;
    case 'back':
      await this.back();
      break;
    default:
      break;
    }
  }

  snapshot() {
    return {
      path: this.current?.path ?? null,
      offset: this.page?.pageStart ?? 0,
      linkId: this.page?.linkId ?? NO_LINK,
      history: this.history.slice(-MAX_HISTORY),
    };
  }

  // Restores a snapshot, dropping entries whose notes are gone.
  async restore(snapshot) {
    const known = new Set(this.vault.paths);
    this.history = (snapshot?.history ?? []).filter((entry) => known.has(entry.path));
    if (snapshot?.path && known.has(snapshot.path)) {
      await this.open(snapshot.path, { offset: snapshot.offset ?? 0, linkId: snapshot.linkId ?? NO_LINK });
    }
  }
}
