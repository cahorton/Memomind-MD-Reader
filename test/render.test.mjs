import assert from 'node:assert/strict';
import test from 'node:test';

import { findHeadingOffset, renderMarkdown, utf8Length } from '../phone/md-reader/md-render.js';
import { resolveNotePath } from '../phone/md-reader/vault-links.js';
import {
  decodeEvent,
  encodeOpen,
  encodeWindow,
  windowFor,
} from '../phone/md-reader/md-protocol.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function linkTexts(rendered) {
  const bytes = encoder.encode(rendered.text);
  return rendered.links.map((link) => decoder.decode(bytes.subarray(link.start, link.end)));
}

test('wikilinks keep their display text and byte spans', () => {
  const rendered = renderMarkdown('See [[Note]], [[Other|alias]] and [[Deep#Part two]].');
  assert.equal(rendered.text, 'See Note, alias and Deep > Part two.');
  assert.deepEqual(linkTexts(rendered), ['Note', 'alias', 'Deep > Part two']);
  assert.deepEqual(rendered.links.map((link) => link.target), [
    { note: 'Note', heading: '' },
    { note: 'Other', heading: '' },
    { note: 'Deep', heading: 'Part two' },
  ]);
});

test('link spans are UTF-8 byte offsets', () => {
  const rendered = renderMarkdown('Café ☕ → [[Ñoño]] 漢字 [[字]]');
  assert.deepEqual(linkTexts(rendered), ['Ñoño', '字']);
  assert.equal(rendered.links[0].start, utf8Length('Café ☕ → '));
});

test('markdown links: notes become links, URLs and images stay text', () => {
  const rendered = renderMarkdown(
    '[Glossary](Glossary.md) [web](https://example.com) ![pic](a.png) [sp](<My Note.md#Top Part>) [enc](My%20Note.md)');
  assert.equal(rendered.text, 'Glossary web [image: pic] sp enc');
  assert.deepEqual(rendered.links.map((link) => link.target), [
    { note: 'Glossary.md', heading: '' },
    { note: 'My Note.md', heading: 'Top Part' },
    { note: 'My Note.md', heading: '' },
  ]);
});

test('reference links resolve from definitions', () => {
  const rendered = renderMarkdown('Read [the guide][g].\n\n[g]: Guide.md');
  assert.equal(rendered.text, 'Read the guide.');
  assert.deepEqual(rendered.links[0].target, { note: 'Guide.md', heading: '' });
});

test('block structure flattens for the glasses', () => {
  const source = [
    '---', 'tags: [a]', '---',
    '# Title', 'Some **bold**, _it_, ~~gone~~ and `[[not a link]]` text. snake_case stays.',
    '', '', '',
    '- item one', '  - nested [[Link]]', '- [ ] todo', '- [x] done', '1. first',
    '> quoted', '> [!note] Heads up',
    '```js', 'const a = 1;', '```',
    '| a | b |', '|---|---|', '| 1 | 2 |',
    '***',
    'Text %% hidden %% here <b>html</b> &amp; more ^block1',
  ].join('\n');
  assert.equal(renderMarkdown(source).text, [
    '# Title', '',
    'Some bold, it, gone and [[not a link]] text. snake_case stays.', '',
    '· item one', '  · nested Link', '[ ] todo', '[x] done', '1. first',
    '| quoted', '| NOTE: Heads up', '',
    '  const a = 1;', '',
    'a | b', '1 | 2', '',
    '----------', '',
    'Text  here html & more',
  ].join('\n'));
});

test('headings are recorded for heading links', () => {
  const rendered = renderMarkdown('# Top\n\nIntro\n\n## Following links\n\nBody');
  const offset = findHeadingOffset(rendered, 'following links');
  assert.equal(rendered.text.slice(offset), '## Following links\n\nBody');
  assert.equal(findHeadingOffset(rendered, 'following-links'), offset);
  assert.equal(findHeadingOffset(rendered, 'missing'), 0);
});

test('empty notes still render something', () => {
  assert.equal(renderMarkdown('---\na: 1\n---\n\n').text, '(empty note)');
});

test('link resolution follows Obsidian rules', () => {
  const paths = ['Home.md', 'Glossary.md', 'Projects/Field Notes.md', 'Projects/Home.md', 'Archive/Old/Glossary.md'];
  assert.equal(resolveNotePath('Glossary', 'Home.md', paths), 'Glossary.md');
  assert.equal(resolveNotePath('glossary.md', 'Home.md', paths), 'Glossary.md');
  assert.equal(resolveNotePath('Home', 'Projects/Field Notes.md', paths), 'Projects/Home.md');
  assert.equal(resolveNotePath('Home', 'Glossary.md', paths), 'Home.md');
  assert.equal(resolveNotePath('Projects/Field Notes', 'Home.md', paths), 'Projects/Field Notes.md');
  assert.equal(resolveNotePath('../Glossary.md', 'Projects/Field Notes.md', paths), 'Glossary.md');
  assert.equal(resolveNotePath('Old/Glossary', 'Home.md', paths), 'Archive/Old/Glossary.md');
  assert.equal(resolveNotePath('Missing', 'Home.md', paths), null);
  assert.equal(resolveNotePath('diagram.png', 'Home.md', paths), null);
  assert.equal(resolveNotePath('', 'Home.md', paths), 'Home.md');
});

test('windows carry overlapping links and stop on a UTF-8 boundary', () => {
  const rendered = renderMarkdown(`${'é'.repeat(5000)} [[A]] tail`);
  const bytes = encoder.encode(rendered.text);
  const window = windowFor(rendered, bytes, 0, 8191);
  assert.equal(window.bytes.length, 8190);
  assert.equal(window.final, false);
  assert.deepEqual(window.links, []);
  const tail = windowFor(rendered, bytes, 9990);
  assert.equal(tail.final, true);
  assert.deepEqual(tail.links, [{ start: 10001, end: 10002, id: 0 }]);
  const message = encodeWindow({ session: 7, ...tail });
  assert.equal(message.length, 13 + 10 + tail.bytes.length);
});

test('open message layout matches the C parser', () => {
  const data = encodeOpen({ session: 1, docId: 2, totalBytes: 3, offset: 4, linkId: 5, historyDepth: 6, title: 'T' });
  assert.deepEqual([...data], [1, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 5, 6, 84]);
  assert.deepEqual(decodeEvent(Uint8Array.from([1, 4, 0, 0, 0, 9])), { type: 'back', session: 9 });
});
