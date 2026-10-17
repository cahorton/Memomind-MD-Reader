// Markdown → glasses display text.
//
// The glasses font has no bold, italics or colour, so Markdown is flattened to
// readable plain text. Links survive as byte spans into that text, which is
// what the glasses need to underline, select and follow them. Offsets are
// UTF-8 byte offsets because the glasses address the text by byte.

const IMAGE_EXTENSIONS = /\.(?:png|jpe?g|gif|webp|svg|bmp|avif|heic)$/iu;
const EXTERNAL_URL = /^[a-z][a-z0-9+.-]*:/iu;
const PLACEHOLDER = /\u0000(\d+)\u0000/gu;

export function utf8Length(text) {
  let length = 0;
  for (let index = 0; index < text.length; index += 1) {
    const code = text.charCodeAt(index);
    if (code < 0x80) length += 1;
    else if (code < 0x800) length += 2;
    else if (code >= 0xd800 && code <= 0xdbff) { length += 4; index += 1; }
    else length += 3;
  }
  return length;
}

// Loose heading comparison shared by [[Note#Heading]] and [x](#heading-slug).
export function headingKey(text) {
  return String(text ?? '').toLowerCase().normalize('NFKD')
    .replace(/[^\p{L}\p{N}]+/gu, '');
}

class Output {
  constructor() {
    this.parts = [];
    this.bytes = 0;
    this.links = [];
    this.headings = [];
    this.lineOpen = false;
    this.blankPending = false;
  }

  append(text) {
    if (!text) return;
    this.parts.push(text);
    this.bytes += utf8Length(text);
  }

  // Starts a new display line, emitting at most one blank line between blocks.
  beginLine() {
    if (this.lineOpen) this.append('\n');
    if (this.blankPending && this.bytes > 0) this.append('\n');
    this.blankPending = false;
    this.lineOpen = true;
  }

  blank() {
    if (this.bytes > 0) this.blankPending = true;
  }

  link(text, target) {
    const start = this.bytes;
    this.append(text);
    if (this.bytes > start) this.links.push({ start, end: this.bytes, target });
  }

  finish() {
    return {
      text: this.parts.join(''),
      links: this.links,
      headings: this.headings,
    };
  }
}

function stripFrontMatter(source) {
  if (!source.startsWith('---\n')) return source;
  const end = source.indexOf('\n---', 4);
  if (end < 0) return source;
  const after = source.indexOf('\n', end + 4);
  return after < 0 ? '' : source.slice(after + 1);
}

function decodeEntities(text) {
  return text
    .replace(/&nbsp;/giu, ' ')
    .replace(/&lt;/giu, '<')
    .replace(/&gt;/giu, '>')
    .replace(/&quot;/giu, '"')
    .replace(/&#39;|&apos;/giu, "'")
    .replace(/&#(\d+);/gu, (_, code) => String.fromCodePoint(Number(code)))
    .replace(/&amp;/giu, '&');
}

// Removes emphasis markers and inline HTML from text without link syntax.
function cleanText(text) {
  return decodeEntities(text
    .replace(/<br\s*\/?>/giu, ' ')
    .replace(/<\/?[a-z][^<>]*>/giu, '')
    .replace(/(\*\*|__)(?=\S)(.+?)(?<=\S)\1/gu, '$2')
    .replace(/~~(?=\S)(.+?)(?<=\S)~~/gu, '$1')
    .replace(/==(?=\S)(.+?)(?<=\S)==/gu, '$1')
    .replace(/(^|[^\p{L}\p{N}*\\])\*(?=\S)(.+?)(?<=\S)\*(?![\p{L}\p{N}*])/gu, '$1$2')
    .replace(/(^|[^\p{L}\p{N}_\\])_(?=\S)(.+?)(?<=\S)_(?![\p{L}\p{N}_])/gu, '$1$2')
    .replace(/\[\^([^\]]+)\]/gu, '[$1]')
    .replace(/\\([\\`*_{}[\]()#+\-.!|~=<>])/gu, '$1'));
}

function splitWikiTarget(inner) {
  const pipe = inner.search(/\\?\|/u);
  const target = (pipe < 0 ? inner : inner.slice(0, pipe)).trim();
  const alias = pipe < 0 ? '' : inner.slice(pipe).replace(/^\\?\|/u, '').trim();
  const hash = target.indexOf('#');
  let note = hash < 0 ? target : target.slice(0, hash);
  let heading = hash < 0 ? '' : target.slice(hash + 1);
  const block = note.indexOf('^');
  if (block >= 0) note = note.slice(0, block);
  heading = heading.replace(/^\^.*$/u, '').replace(/#/gu, ' ').trim();
  return { note: note.trim(), heading, alias };
}

function noteDisplayName(note) {
  return note.replace(/\.md$/iu, '');
}

function parseMarkdownTarget(raw) {
  let url = raw.trim();
  if (url.startsWith('<')) url = url.slice(1, url.indexOf('>') < 0 ? undefined : url.indexOf('>'));
  else url = url.split(/\s+/u)[0];
  if (!url || EXTERNAL_URL.test(url)) return null;
  let decoded = url;
  try { decoded = decodeURIComponent(url); } catch { /* keep the raw path */ }
  const hash = decoded.indexOf('#');
  const note = hash < 0 ? decoded : decoded.slice(0, hash);
  const heading = hash < 0 ? '' : decoded.slice(hash + 1);
  if (note && IMAGE_EXTENSIONS.test(note)) return null;
  return { note, heading };
}

// Inline pass: pulls link-like tokens out, cleans the remaining text, then
// writes text and link spans to the output in order.
function writeInline(output, line, references) {
  const tokens = [];
  const hold = (token) => `\u0000${tokens.push(token) - 1}\u0000`;
  let text = line
    .replace(/(`+)(.+?)\1/gu, (_, __, code) => hold({ kind: 'text', text: code }))
    .replace(/!\[\[([^\]]+)\]\]/gu, (_, inner) => {
      const { note, alias } = splitWikiTarget(inner);
      if (IMAGE_EXTENSIONS.test(note) || /\.\w{2,4}$/u.test(note) && !/\.md$/iu.test(note)) {
        return hold({ kind: 'text', text: `[${alias || note}]` });
      }
      return hold({ kind: 'embed', text: alias || noteDisplayName(note), target: { note, heading: '' } });
    })
    .replace(/\[\[([^\]]+)\]\]/gu, (_, inner) => {
      const { note, heading, alias } = splitWikiTarget(inner);
      const shown = alias || (note && heading ? `${noteDisplayName(note)} > ${heading}`
        : noteDisplayName(note) || heading);
      return hold({ kind: 'link', text: shown, target: { note, heading } });
    })
    .replace(/!\[([^\]]*)\]\(([^)]*)\)/gu, (_, alt) => hold({ kind: 'text', text: alt ? `[image: ${alt}]` : '[image]' }))
    .replace(/\[([^\]]+)\]\(((?:<[^>]*>|[^()\s]|\([^()]*\))*(?:\s+"[^"]*")?)\)/gu, (_, label, url) => {
      const target = parseMarkdownTarget(url);
      return hold(target ? { kind: 'link', text: label, target } : { kind: 'plain', text: label });
    })
    .replace(/\[([^\]]+)\]\[([^\]]*)\]/gu, (match, label, ref) => {
      const url = references.get((ref || label).toLowerCase());
      if (url === undefined) return match;
      const target = parseMarkdownTarget(url);
      return hold(target ? { kind: 'link', text: label, target } : { kind: 'plain', text: label });
    })
    .replace(/<((?:https?|mailto):[^>\s]+)>/giu, (_, url) => hold({ kind: 'text', text: url }));
  text = cleanText(text);
  // A link label may itself hold a code span placeholder.
  const flat = (value) => value.replace(PLACEHOLDER, (_, index) => tokens[Number(index)].text);
  let cursor = 0;
  for (const match of text.matchAll(PLACEHOLDER)) {
    output.append(text.slice(cursor, match.index));
    const token = tokens[Number(match[1])];
    if (token.kind === 'text') {
      output.append(token.text);
    } else if (token.kind === 'plain') {
      output.append(flat(cleanText(token.text)));
    } else if (token.kind === 'embed') {
      output.append('Embed: ');
      output.link(flat(cleanText(token.text)), token.target);
    } else {
      output.link(flat(cleanText(token.text)), token.target);
    }
    cursor = match.index + match[0].length;
  }
  output.append(text.slice(cursor));
}

function collectReferences(lines) {
  const references = new Map();
  const kept = [];
  for (const line of lines) {
    const match = /^\s{0,3}\[([^\]^]+)\]:\s*(\S+)/u.exec(line);
    if (match) references.set(match[1].toLowerCase(), match[2]);
    else kept.push(line);
  }
  return { references, lines: kept };
}

export function renderMarkdown(source) {
  const output = new Output();
  let text = String(source ?? '')
    .replace(/^﻿/u, '')
    .replace(/\r\n?/gu, '\n')
    .replace(/\t/gu, '    ')
    .replace(/ /gu, ' ');
  text = stripFrontMatter(text)
    .replace(/%%[\s\S]*?%%/gu, '')
    .replace(/<!--[\s\S]*?-->/gu, '');
  const { references, lines } = collectReferences(text.split('\n'));
  let fence = null;
  for (const rawLine of lines) {
    const line = rawLine.replace(/\s+$/u, '');
    const fenceMatch = /^\s*(`{3,}|~{3,})/u.exec(line);
    if (fence) {
      if (fenceMatch && fenceMatch[1][0] === fence[0] && fenceMatch[1].length >= fence.length) {
        fence = null;
        output.blank();
      } else {
        output.beginLine();
        output.append(`  ${line}`);
      }
      continue;
    }
    if (fenceMatch) {
      fence = fenceMatch[1];
      output.blank();
      continue;
    }
    if (!line.trim()) {
      output.blank();
      continue;
    }
    const heading = /^\s{0,3}(#{1,6})\s+(.*?)(?:\s+#+)?\s*$/u.exec(line);
    if (heading) {
      output.blank();
      output.beginLine();
      const start = output.bytes;
      output.append(`${heading[1]} `);
      const body = heading[2].replace(/\s+\^[\w-]+$/u, '');
      writeInline(output, body, references);
      output.headings.push({
        text: cleanText(body.replace(/\[\[([^\]|]*\|)?([^\]]*)\]\]/gu, '$2')),
        level: heading[1].length,
        offset: start,
      });
      output.blank();
      continue;
    }
    if (/^\s{0,3}([-*_])(?:\s*\1){2,}\s*$/u.test(line)) {
      output.blank();
      output.beginLine();
      output.append('----------');
      output.blank();
      continue;
    }
    if (/^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)+\|?\s*$/u.test(line)) continue;
    output.beginLine();
    let body = line.replace(/\s+\^[\w-]+$/u, '');
    const quote = /^\s*((?:>\s?)+)(.*)$/u.exec(body);
    if (quote) {
      const depth = quote[1].replace(/\s/gu, '').length;
      output.append('| '.repeat(depth));
      body = quote[2];
      const callout = /^\[!(\w+)\][+-]?\s*(.*)$/u.exec(body);
      if (callout) {
        output.append(`${callout[1].toUpperCase()}${callout[2] ? ': ' : ''}`);
        body = callout[2];
      }
    }
    const item = /^(\s*)([-*+]|\d+[.)])\s+(\[([ xX])\]\s+)?(.*)$/u.exec(body);
    if (item) {
      const indent = '  '.repeat(Math.min(6, Math.floor(item[1].length / 2)));
      const marker = item[3] ? (item[4] === ' ' ? '[ ] ' : '[x] ')
        : /\d/u.test(item[2]) ? `${item[2]} ` : '· ';
      output.append(indent + marker);
      body = item[5];
    } else if (/^\s*\|.*\|\s*$/u.test(body)) {
      body = body.trim().replace(/^\|/u, '').replace(/\|$/u, '')
        .split(/(?<!\\)\|/u).map((cell) => cell.trim()).join(' | ');
    } else {
      body = body.replace(/^\s+/u, '');
    }
    writeInline(output, body, references);
  }
  const rendered = output.finish();
  if (!rendered.text.trim()) {
    return { text: '(empty note)', links: [], headings: [] };
  }
  return rendered;
}

export function findHeadingOffset(rendered, heading) {
  const key = headingKey(heading);
  if (!key) return 0;
  const match = rendered.headings.find((entry) => headingKey(entry.text) === key);
  return match ? match.offset : 0;
}
