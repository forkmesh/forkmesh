# Writing with AI in notes

Part of [the App docs](README.md). In the web Notes editor you can select text
and have AI clean it up, check its grammar or rewrite it from an instruction
you type, then put the result back in the note. With nothing selected, the
instruction writes new text at the cursor. The desktop has the same quick
actions but runs them on the local agent
([desktop notes](../desktop/notes.md#rewriting-a-selection)); the phone apps do
not have it yet ([platform differences](notes.md#the-same-features-on-every-platform)).

Code: `app/src/notes_assist.py` (actions, prompt, limits), routed from
`notes.handle()` in `app/src/notes.py`; `assist_config()` and `assist_call()`
on `_NotesRuntime` in `app/src/entry.py`; the editor in
`app/public/dashboard/js/09-notes.js` (`openNoteAssist()`, `runNoteAssist()`,
`applyNoteAssist()`) and the popover in
`app/public/dashboard/partials/views/notes.html`.

## In the editor

- **Ask AI** appears beside **Comment** when you select text you can edit, and
  the sparkles button in the format bar (⌘J / Ctrl+J) opens the same popover
  for the selection, or for the cursor when nothing is selected.
- The popover shows the selected text, a field for your own instruction and
  the quick actions: Clean up, Check grammar, Make clearer, Make shorter, Make
  longer, Professional tone, Friendly tone, Bullet list and Summarize. Quick
  actions need a selection, so they are hidden in the cursor case.
- The answer appears in an editable box. **Replace selection** (or **Insert**
  at the cursor) puts it in the note; **Insert below** adds it as a new
  paragraph after the selection's line; **Try again** asks again; **Discard**
  or Escape closes it. The edit is an ordinary input, so autosave, the live
  room and version history treat it like typing.
- A note that changed meanwhile is handled by finding the selected text again;
  if it is gone, the suggestion stays in the box with a notice.
- Inserted at the cursor, the text gets a space or a blank line where it would
  otherwise run into the next word or line.
- The buttons are hidden for viewers and when `GET /api/notes/assist` reports
  `ready: false`.

## API

`GET /api/notes/assist` (signed in) answers
`{ok, ready, actions: [{id, label}], remaining, maxText, maxPrompt}`.

`POST /api/notes/assist` takes `{action, text, prompt, before, after}`:

| Field | Rule |
| --- | --- |
| `action` | One of the quick-action ids, or `custom` |
| `text` | The selection, up to 6,000 characters; required unless `custom` |
| `prompt` | Up to 500 characters; required for `custom`, an extra request otherwise |
| `before`, `after` | Up to 600 characters of context on each side |

It answers `{ok, text, action, model}`, or `400` (`invalid_action`,
`text_required`, `prompt_required`, `text_too_long`, `prompt_too_long`), `401`,
`429 assist_daily_limit`, `503 assist_unavailable` or `502 assist_failed` with
the provider's `detail`.

The request goes through the notes runtime, so a desktop-signed request works
like any other notes call ([desktop notes](../desktop/notes.md#authentication)).

## Provider, limits and privacy

- The model and credentials are the instance's [AI review](ai-review.md)
  settings: Cloudflare Workers AI under the instance's own account, with the
  approved model chosen there. The separate **Writing help in Notes** switch
  (`notesAssist`, on unless turned off) decides whether notes may use it, so
  pull request reviews can stay off. Without an account ID and token (or the
  `AI` binding) it is not ready.
- 100 requests per person per UTC day, counted in `ai_review_budget` under
  `notes-assist:<account>` before the provider is called.
- Only the selection, the context around it and the instruction are sent, and
  only when the person asks. Nothing is stored: neither the text nor the
  answer is logged.
- The note's text sits between per-request random markers; marker-like text
  inside it is stripped, and the system prompt treats it as material, not
  instructions. The answer is trimmed, and a code fence around the whole
  answer is removed unless the selection was itself fenced. The answer is a
  suggestion the person reads before it reaches the note.

## Tests

- `app/tests/test_notes_assist.py`: the action list and `ready`, sessions and
  methods, a quick action and its prompt with context, custom prompts with and
  without a selection, the 400 cases before any provider call, the 503 and
  502 answers, the per-account daily limit, and markers inside the note.
- `app/browser_tests/tests/dashboard-notes-assist.spec.js` drives the built
  page with mocked APIs: Ask AI on a selection with Check grammar and Replace,
  a typed instruction with Insert below, Ctrl+J at the cursor with Insert, and
  the buttons hidden when the provider is not ready.
- `desktop/tests/test_note_tools.cpp` `testAiSelection`: the desktop's quick
  actions, trimmed ranges, the selection prompt and its context limit, and
  reading the answer.
