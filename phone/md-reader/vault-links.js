// Obsidian-style link resolution over the vault's note paths.

export function stripNoteExtension(path) {
  return path.replace(/\.md$/iu, '');
}

export function basename(path) {
  const slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.slice(slash + 1);
}

export function dirname(path) {
  const slash = path.lastIndexOf('/');
  return slash < 0 ? '' : path.slice(0, slash);
}

function normalizePath(path) {
  const parts = [];
  for (const part of path.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') parts.pop();
    else parts.push(part);
  }
  return parts.join('/');
}

const key = (path) => stripNoteExtension(path).toLowerCase();

// Returns the matching note path, the current path for a same-note link, or
// null when nothing in the vault matches.
export function resolveNotePath(target, fromPath, notePaths) {
  const wanted = String(target ?? '').trim().replace(/^\/+/u, '');
  if (!wanted) return fromPath ?? null;
  if (/\.\w{1,5}$/u.test(wanted) && !/\.md$/iu.test(wanted)) return null;
  const byKey = new Map();
  for (const path of notePaths) {
    if (!byKey.has(key(path))) byKey.set(key(path), path);
  }
  if (wanted.includes('/')) {
    const relative = normalizePath(`${dirname(fromPath ?? '')}/${wanted}`);
    const candidates = [relative, normalizePath(wanted)];
    for (const candidate of candidates) {
      const hit = byKey.get(key(candidate));
      if (hit) return hit;
    }
    const suffix = `/${key(normalizePath(wanted))}`;
    const matches = notePaths.filter((path) => `/${key(path)}`.endsWith(suffix));
    return pickClosest(matches, fromPath);
  }
  const name = key(wanted);
  const matches = notePaths.filter((path) => key(basename(path)) === name);
  return pickClosest(matches, fromPath);
}

function pickClosest(matches, fromPath) {
  if (matches.length === 0) return null;
  const folder = dirname(fromPath ?? '');
  const sameFolder = matches.find((path) => dirname(path) === folder);
  if (sameFolder) return sameFolder;
  return [...matches].sort((left, right) =>
    left.split('/').length - right.split('/').length || left.length - right.length)[0];
}

export function noteTitle(path) {
  return stripNoteExtension(basename(path));
}

// Alphabetical, ignoring case and accents, with numbers in natural order
// (s2 before s10).
const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });
export function compareNames(left, right) {
  return collator.compare(left, right) || (left < right ? -1 : left > right ? 1 : 0);
}

// One level of the vault tree: sub-folders (with how many notes they hold)
// and the notes directly inside `folder` ('' is the vault root).
export function listFolder(paths, folder = '') {
  const prefix = folder ? `${folder}/` : '';
  const folders = new Map();
  const notes = [];
  for (const path of paths) {
    if (!path.startsWith(prefix)) continue;
    const rest = path.slice(prefix.length);
    const slash = rest.indexOf('/');
    if (slash < 0) {
      notes.push(path);
    } else {
      const name = rest.slice(0, slash);
      folders.set(name, (folders.get(name) ?? 0) + 1);
    }
  }
  return {
    folders: [...folders]
      .map(([name, count]) => ({ name, path: prefix + name, count }))
      .sort((left, right) => compareNames(left.name, right.name)),
    notes: notes.sort((left, right) => compareNames(noteTitle(left), noteTitle(right))),
  };
}

// The folder to start browsing in: a vault whose notes all sit in one top
// folder (a zipped "ObsidianVault/") opens inside it.
export function browseRoot(paths) {
  let root = '';
  for (;;) {
    const level = listFolder(paths, root);
    if (level.notes.length !== 0 || level.folders.length !== 1) return root;
    root = level.folders[0].path;
  }
}

// Folder prefix for a zip's notes: none when they already share one top
// folder, otherwise the zip's own name so each vault stays one folder.
export function zipPrefix(entryNames, zipName) {
  const tops = new Set(entryNames.map((name) => (name.includes('/') ? name.slice(0, name.indexOf('/')) : '')));
  if (tops.size === 1 && !tops.has('')) return '';
  return `${zipName.replace(/\.zip$/iu, '')}/`;
}
