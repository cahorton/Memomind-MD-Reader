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
