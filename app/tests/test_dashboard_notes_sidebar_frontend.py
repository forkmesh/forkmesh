#!/usr/bin/env python3
"""The notes sidebar states each note's publish status, sharing, and reads.

The list used to render only "v<n> · <role> · <when>", which answered neither
of the questions a note list exists to answer: is this published, and who else
can see it? A note that had never reached the cloud looked exactly like one
live on the web — the reported symptom was a "published" note that was nowhere
on the web with nothing in the UI saying so.

These contracts pin the three labels the row is built from, and the listing
payload that feeds them (`shares`, `views` and `readers` now come back from
GET /api/notes, so no row needs a per-note fetch to label itself).
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# dashboard.js plus its per-page chunks (notes is one of them).
BUNDLE = "\n".join(
    path.read_text(encoding="utf-8")
    for path in [ROOT / "public" / "dashboard.js"]
    + sorted((ROOT / "public").glob("dashboard-page-*.js")))
NOTES_JS = (ROOT / "public" / "dashboard" / "js" / "09-notes.js").read_text(
    encoding="utf-8")
NOTES_API = (ROOT / "src" / "notes.py").read_text(encoding="utf-8")


def _region(text, start, end):
    return text[text.index(start):text.index(end)]


def test_every_row_states_publish_status_shares_and_reads():
    row = _region(NOTES_JS, "function renderNoteRow(", "function renderNoteList(")
    # Publish state is named on every row (as the icon's label) and the meta
    # line carries the read count.
    assert "const status = noteStatusLabel(note);" in row
    assert "noteViewsLabel(note)" in row
    # The sharing line is its own line, so it is never truncated away.
    assert "noteSharedWithLabel(note)" in row
    # A published note is visually distinct, not just differently worded. The
    # icon is the link blue rather than green: green is the dashboard's
    # primary-action colour and no longer marks state.
    assert 'note.visibility === "public"' in row
    assert "notes-row-vis" in row
    view = (ROOT.parent / "marketplace" / "packages" / "notes" / "content" / "web"
            / "view.html").read_text(encoding="utf-8")
    assert '.notes-row-vis[data-visibility="public"] { color: var(--nt-accent); }' in view

    status = _region(NOTES_JS, "function noteStatusLabel(", "// \"12 views\"")
    assert 'if (note.visibility === "public") return "Public";' in status
    assert 'return (note.shares || []).length ? "Shared" : "Private";' in status

    views = _region(NOTES_JS, "function noteViewsLabel(", "function noteSharedWithLabel(")
    # An unread draft is not labelled "0 views".
    assert "if (views <= 0) return \"\";" in views
    assert "readers > 1 ? `${counted} from ${readers} readers` : counted" in views

    shared = _region(NOTES_JS, "function noteSharedWithLabel(", "function renderNoteList(")
    assert '`${share.name} (${share.role || "viewer"})`' in shared
    assert '"Not shared with anyone"' in shared

    # The shipped bundle is the built artifact the dashboard actually loads.
    for fragment in ("function noteStatusLabel(", "function noteViewsLabel(",
                     "function noteSharedWithLabel("):
        assert fragment in BUNDLE


def test_listing_supplies_the_labels_without_a_per_row_fetch():
    listing = _region(NOTES_API, "async def _list(", "async def _create(")
    assert "shares = await _shares_by_note(runtime, note_ids)" in listing
    assert "views = await _views_by_note(runtime, note_ids)" in listing
    assert 'note["shares"] = shares.get(row["note_id"], [])' in listing
    # Batched, not one query per row: a 500-note listing must stay affordable.
    assert "for row in rows" in listing
    assert "await _shares(runtime" not in listing
    assert "await _views(runtime" not in listing


def test_only_anonymous_reads_of_a_published_note_are_counted():
    handler = _region(NOTES_API, 'if method == "GET":\n        # Only anonymous',
                      "if not account_bi:\n        return _response(runtime, "
                      '{"error": "invalid_session"}, status=401)\n    if method == "DELETE"')
    assert 'if role == "public":' in handler
    assert "await _record_view(runtime, note_id)" in handler

    recorder = _region(NOTES_API, "async def _record_view(", "async def _links(")
    # A runtime that cannot derive a reader key leaves the count alone rather
    # than collapsing every reader into one.
    assert "if not viewer_key:" in recorder
    assert "return" in recorder
    # Counting a read must never be able to fail the page it was counting.
    assert "except Exception:" in recorder
