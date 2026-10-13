#!/usr/bin/env bash
# name: Scrub a term from source and history
# description: Replace a word in every tracked file and in every commit, message and author on all branches, then prune the old objects. Run it from a search with Scrub….
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: SCRUB_TERM=<word> [SCRUB_REPLACEMENT=<text>] [SCRUB_HISTORY=0] scrub-term.sh

Replaces SCRUB_TERM (not case-sensitive) with SCRUB_REPLACEMENT (default
[redacted]) in the repository in the current folder:

  1. every tracked text file, committed as "Scrub a term from the source";
  2. unless SCRUB_HISTORY=0: every file, commit message, author, committer and
     tag on every branch, tag and ref except refs/stash, then expires the
     reflogs (refs/stash's is kept) and runs git gc --prune=now so the old
     objects are gone.

Before rewriting history it writes every ref to a git bundle in
SCRUB_BACKUP_DIR (default $FORKMESH_DATA_DIR/scrub-backups). The bundle still
holds the term: delete it once the result is checked. Remotes and other clones
keep the old history until they are replaced or force-pushed.

In ForkMesh, open the search for the word and press Scrub… to run this.
USAGE
}

term="${SCRUB_TERM:-}"
replacement="${SCRUB_REPLACEMENT-[redacted]}"
history="${SCRUB_HISTORY:-1}"

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi
if [ -z "$term" ]; then
  usage >&2
  echo >&2
  echo "SCRUB_TERM is not set: nothing scrubbed." >&2
  exit 2
fi
if [ "${#term}" -lt 3 ]; then
  echo "Refusing: \"$term\" is shorter than 3 characters and would rewrite too much." >&2
  exit 2
fi
case "$term$replacement" in
  *$'\n'*|*'<'*|*'>'*)
    echo "Refusing: the term and replacement cannot hold a newline, < or >." >&2
    exit 2
    ;;
esac
if [ -n "$replacement" ] && printf '%s' "$replacement" | grep -qiF -- "$term"; then
  echo "Refusing: the replacement contains the term itself." >&2
  exit 2
fi

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "Not inside a git checkout: $(pwd)" >&2
  exit 2
}
cd "$(git rev-parse --show-toplevel)"
here="$(pwd -P)"
git symbolic-ref -q HEAD >/dev/null || {
  echo "Refusing: HEAD is detached. Check out a branch first." >&2
  exit 2
}
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "Refusing: this checkout has uncommitted changes. Commit or move them first." >&2
  git status --short --untracked-files=no | head -20 >&2
  exit 2
fi

export SCRUB_TERM="$term" SCRUB_REPLACEMENT="$replacement"
python="$(command -v python3 || true)"
[ -n "$python" ] || { echo "python3 is needed to scrub." >&2; exit 2; }

echo "Scrubbing a ${#term}-character term from $(pwd)"

source_files="$("$python" -I -c '
import os, re, subprocess, sys
term = os.environb[b"SCRUB_TERM"]
repl = os.environb[b"SCRUB_REPLACEMENT"]
pattern = re.compile(re.escape(term), re.IGNORECASE)
listed = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
changed = 0
for name in filter(None, listed.split(b"\0")):
    if os.path.islink(name) or not os.path.isfile(name):
        continue
    with open(name, "rb") as handle:
        body = handle.read()
    if b"\0" in body:
        continue
    new, hits = pattern.subn(repl, body)
    if hits:
        with open(name, "wb") as handle:
            handle.write(new)
        subprocess.run(["git", "add", "--", name], check=True)
        changed += 1
print(changed)
')"
if [ "$source_files" -gt 0 ]; then
  git commit -q -m "Scrub a term from the source"
  echo "Source: replaced it in $source_files file(s) and committed."
else
  echo "Source: no tracked file holds it."
fi

named_paths="$(git ls-files | grep -ciF -- "$term" || true)"
if [ "$named_paths" -gt 0 ]; then
  echo "Note: $named_paths tracked path(s) have it in their name; rename them yourself:"
  git ls-files | grep -iF -- "$term" | head -20
fi

if [ "$history" = "0" ]; then
  echo "History: left alone (SCRUB_HISTORY=0)."
  exit 0
fi

mapfile -t refs < <(git for-each-ref --format='%(refname)' | grep -vx 'refs/stash')
[ "${#refs[@]}" -gt 0 ] || { echo "No refs to rewrite."; exit 0; }

backup_dir="${SCRUB_BACKUP_DIR:-${FORKMESH_DATA_DIR:-$HOME/.local/share/forkmesh}/scrub-backups}"
mkdir -p "$backup_dir"
backup="$backup_dir/$(basename "$(pwd)")-$(date +%Y%m%d-%H%M%S).bundle"
git bundle create -q "$backup" --all
echo "Backup of every ref before the rewrite: $backup"

echo "History: rewriting ${#refs[@]} ref(s)…"
git fast-export --signed-tags=strip --reencode=yes "${refs[@]}" \
  | "$python" -I -c '
import os, re, sys
term = os.environb[b"SCRUB_TERM"]
repl = os.environb[b"SCRUB_REPLACEMENT"]
pattern = re.compile(re.escape(term), re.IGNORECASE)
source = sys.stdin.buffer
sink = sys.stdout.buffer
blobs = 0
idents = 0
while True:
    line = source.readline()
    if not line:
        break
    if line.startswith(b"data "):
        body = source.read(int(line[5:]))
        if b"\0" not in body:
            body, hits = pattern.subn(repl, body)
            blobs += hits > 0
        sink.write(b"data %d\n" % len(body))
        sink.write(body)
        continue
    if line.startswith((b"author ", b"committer ", b"tagger ")):
        line, hits = pattern.subn(repl, line)
        idents += hits > 0
    sink.write(line)
sink.flush()
print("History: changed %d file version(s) or message(s) and %d author line(s)." % (blobs, idents), file=sys.stderr)
' \
  | git fast-import --force --quiet

for ref in "${refs[@]}" HEAD; do
  git reflog expire --expire=now --expire-unreachable=now "$ref" 2>/dev/null || true
done
git worktree list --porcelain | sed -n 's/^worktree //p' | while IFS= read -r tree; do
  [ "$(cd "$tree" 2>/dev/null && pwd -P)" = "$here" ] && continue
  git -C "$tree" reflog expire --expire=now --expire-unreachable=now HEAD 2>/dev/null || true
  echo "Note: worktree $tree still has the old files checked out; refresh or recreate it."
done
git gc -q --prune=now

left="$(git cat-file --batch-all-objects --batch-check='%(objectname) %(objecttype)' \
  | awk '$2=="blob"||$2=="commit"||$2=="tag"{print $1}' \
  | git cat-file --batch | grep -aciF -- "$term" || true)"
if [ "$left" -gt 0 ]; then
  echo "Still found in $left object line(s): refs/stash, a detached worktree or a binary file still holds it."
else
  echo "Verified: no object in this repository holds it."
fi
if git rev-parse -q --verify refs/stash >/dev/null; then
  echo "Note: refs/stash was left alone; stashes may still hold it."
fi
if [ -n "$(git remote)" ]; then
  echo "Note: remotes ($(git remote | paste -sd, -)) still have the old history until you force-push or replace them."
fi
echo "When you have checked the result, delete $backup."
