// The vault: imported .md files and zipped vaults kept in the App's private
// file storage, exposed as one set of note paths.

import { noteTitle } from './vault-links.js';

const MAX_DIRECTORY_BYTES = 8 * 1024 * 1024;
const MAX_NOTE_BYTES = 4 * 1024 * 1024;
const TICKET_RETRY_DELAYS_MS = [100, 300];
const decoder = new TextDecoder('utf-8');

export class Vault {
  constructor(gm) {
    this.gm = gm;
    this.sources = [];
    this.notes = new Map();
    this.textCache = new Map();
  }

  get paths() {
    return [...this.notes.keys()];
  }

  async reload() {
    const result = await this.gm.files.list();
    const files = (Array.isArray(result?.files) ? result.files : [])
      .filter((file) => /\.(md|markdown|zip)$/iu.test(file.name ?? ''));
    this.sources = [];
    this.notes = new Map();
    this.textCache.clear();
    for (const file of files) {
      const source = { fileId: file.fileId, name: file.name, size: file.size, notes: 0, error: '' };
      this.sources.push(source);
      try {
        if (/\.zip$/iu.test(file.name)) {
          const archive = await ZipArchive.open(this, file);
          for (const entry of archive.entries.values()) {
            if (!/\.(md|markdown)$/iu.test(entry.name) || isHiddenPath(entry.name)) continue;
            this.addNote(entry.name, { file, archive, entry: entry.name }, source);
          }
        } else {
          this.addNote(file.name, { file }, source);
        }
      } catch (error) {
        source.error = error.message;
      }
    }
    return this;
  }

  addNote(path, location, source) {
    const clean = path.replace(/\.markdown$/iu, '.md');
    if (this.notes.has(clean)) return;
    this.notes.set(clean, { path: clean, title: noteTitle(clean), ...location });
    source.notes += 1;
  }

  async pick(kind) {
    const result = await this.gm.files.pick(kind === 'zip'
      ? { extensions: ['zip'], allowMultiple: false }
      : { extensions: ['md', 'markdown'], allowMultiple: true });
    return Array.isArray(result?.files) ? result.files.length : 0;
  }

  async removeSource(fileId) {
    const result = await this.gm.files.delete(fileId);
    if (result?.deleted !== true) throw new Error('The app did not remove the file');
  }

  async readNote(path) {
    if (this.textCache.has(path)) return this.textCache.get(path);
    const note = this.notes.get(path);
    if (!note) throw new Error(`Note not found: ${path}`);
    const bytes = note.archive
      ? await note.archive.read(note.entry, MAX_NOTE_BYTES)
      : await this.readRange(note.file.fileId, 0, note.file.size);
    const text = decoder.decode(bytes);
    this.textCache.set(path, text);
    if (this.textCache.size > 32) this.textCache.delete(this.textCache.keys().next().value);
    return text;
  }

  async openRead(fileId, options) {
    for (let attempt = 0; ; attempt += 1) {
      try {
        return await this.gm.files.openRead(fileId, options);
      } catch (error) {
        // A short-lived file ticket can expire across an Android lifecycle race.
        if (attempt >= TICKET_RETRY_DELAYS_MS.length) throw error;
        await new Promise((resolve) => setTimeout(resolve, TICKET_RETRY_DELAYS_MS[attempt]));
      }
    }
  }

  async readRange(fileId, offset, length) {
    if (length === 0) return new Uint8Array(0);
    const opened = await this.openRead(fileId, { offset, length });
    const output = new Uint8Array(length);
    const reader = opened.stream.getReader();
    let written = 0;
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!(value instanceof Uint8Array) || written + value.length > length) {
          throw new Error('The app returned an invalid file stream');
        }
        output.set(value, written);
        written += value.length;
      }
    } finally {
      reader.releaseLock();
    }
    if (written !== length) throw new Error('The file ended early');
    return output;
  }
}

function isHiddenPath(path) {
  return path.split('/').some((part) => part.startsWith('.') || part === '__MACOSX');
}

class ZipArchive {
  constructor(vault, file, entries) {
    this.vault = vault;
    this.file = file;
    this.entries = entries;
  }

  static async open(vault, file) {
    const tailLength = Math.min(file.size, 65_557);
    const tail = await vault.readRange(file.fileId, file.size - tailLength, tailLength);
    const eocd = findSignatureBackward(tail, 0x06054b50);
    if (eocd < 0 || eocd + 22 > tail.length) throw new Error('Not a ZIP file');
    const view = new DataView(tail.buffer, tail.byteOffset, tail.byteLength);
    const entryCount = view.getUint16(eocd + 10, true);
    const directorySize = view.getUint32(eocd + 12, true);
    const directoryOffset = view.getUint32(eocd + 16, true);
    if (entryCount === 0xffff || directorySize === 0xffffffff || directoryOffset === 0xffffffff) {
      throw new Error('ZIP64 archives are not supported');
    }
    if (directorySize > MAX_DIRECTORY_BYTES || directoryOffset + directorySize > file.size) {
      throw new Error('ZIP directory is too large');
    }
    const directory = await vault.readRange(file.fileId, directoryOffset, directorySize);
    const entries = new Map();
    let cursor = 0;
    for (let index = 0; index < entryCount; index += 1) {
      if (cursor + 46 > directory.length || readU32(directory, cursor) !== 0x02014b50) {
        throw new Error('ZIP directory is malformed');
      }
      const flags = readU16(directory, cursor + 8);
      const method = readU16(directory, cursor + 10);
      const compressedSize = readU32(directory, cursor + 20);
      const size = readU32(directory, cursor + 24);
      const nameLength = readU16(directory, cursor + 28);
      const extraLength = readU16(directory, cursor + 30);
      const commentLength = readU16(directory, cursor + 32);
      const localOffset = readU32(directory, cursor + 42);
      const name = decoder.decode(directory.subarray(cursor + 46, cursor + 46 + nameLength));
      if (!name.endsWith('/') && !entries.has(name)) {
        entries.set(name, { name, flags, method, compressedSize, size, localOffset });
      }
      cursor += 46 + nameLength + extraLength + commentLength;
    }
    return new ZipArchive(vault, file, entries);
  }

  async read(path, limit) {
    const entry = this.entries.get(path);
    if (!entry) throw new Error(`Missing from ZIP: ${path}`);
    if ((entry.flags & 1) !== 0) throw new Error('Encrypted ZIP entries are not supported');
    if (entry.size > limit) throw new Error(`Note is too large: ${path}`);
    const header = await this.vault.readRange(this.file.fileId, entry.localOffset, 30);
    if (readU32(header, 0) !== 0x04034b50) throw new Error('ZIP entry header is malformed');
    const dataOffset = entry.localOffset + 30 + readU16(header, 26) + readU16(header, 28);
    const compressed = await this.vault.readRange(this.file.fileId, dataOffset, entry.compressedSize);
    if (entry.method === 0) return compressed;
    if (entry.method !== 8) throw new Error(`Unsupported ZIP compression method ${entry.method}`);
    if (typeof DecompressionStream !== 'function') {
      throw new Error('This WebView cannot unzip files');
    }
    const stream = new Blob([compressed]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }
}

function readU16(bytes, offset) {
  return bytes[offset] | (bytes[offset + 1] << 8);
}

function readU32(bytes, offset) {
  return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) |
    (bytes[offset + 3] << 24)) >>> 0;
}

function findSignatureBackward(bytes, signature) {
  for (let offset = bytes.length - 4; offset >= 0; offset -= 1) {
    if (readU32(bytes, offset) === signature) return offset;
  }
  return -1;
}
