# Workflow steps beyond HTTP and integrations

Part of [the App docs](README.md); the engine is described in
[workflows.md](workflows.md). These core steps give Workflows what Apple's
Shortcuts app offers for building automations (scripting, text, numbers,
lists, dates), plus a script step for anything else
([workflow-scripts.md](workflow-scripts.md)). They are declared in
`app/src/workflow_actions.py` (`NODES`), merged into `workflows.CORE_NODES`,
and run by `Execution.run_action` in `app/src/workflows.py`.

Device actions from Shortcuts (flashlight, Bluetooth, screen brightness,
Ask for input, Show alert) are not here: a workflow also runs on the server
from a webhook or schedule, where no phone or person is present. Messages and
notifications go through the integrations (Slack, Telegram, email, ...).

## The steps

Text, number, date and URL steps read one value per item (an expression such
as `{{ $json.text }}`) and write the result to the item under **Save as**
(a dotted path; the rest of the item is kept).

| Group | Step | Type | What it does |
| --- | --- | --- | --- |
| Scripting | Run script | `core.code` | JavaScript once for all items or once per item ([workflow-scripts.md](workflow-scripts.md)) |
| | Set variable | `core.variable` | Set, add to a list, or add to a number; later steps read `{{ $vars.name }}` |
| | Choose by value | `core.switch` | Outputs `case1`, `case2`, `case3`, `otherwise`; first case whose text equals the value wins, empty cases never match |
| | Repeat | `core.repeat` | Each item 0 to 100 times with a count field (default `repeatIndex`, from 1) |
| | Wait | `core.wait` | Pause up to 60 seconds, and never past the run's time limit |
| | Run workflow | `core.runWorkflow` | Run another of your workflows (ID, or name ignoring case) with each item as its trigger data |
| | Stop workflow | `core.stop` | End the run as a success (later steps are skipped, the report gets `stopped`) or with an error message |
| | Comment | `core.comment` | A note; items pass through |
| Text | Change case | `core.textCase` | UPPER, lower, Capitalize first letter, Capitalize Every Word |
| | Replace text | `core.textReplace` | Plain or regular expression, optionally ignoring case; `$1`, `$&`, `$<name>` in the replacement |
| | Match text | `core.textMatch` | All matches, first match, groups of the first match, or whether it matches |
| | Split text | `core.textSplit` | At new lines, spaces, commas, every character or custom text |
| | Combine text | `core.textCombine` | Join a list with new lines, spaces, commas, nothing or custom text |
| | Encode text | `core.textEncode` | Base64, URL and HTML encode and decode (UTF-8) |
| | Hash text | `core.hash` | MD5, SHA-1, SHA-256, SHA-512; an HMAC when a key is set; hex or Base64 |
| | Get from text | `core.textExtract` | Links, email addresses, numbers, phone numbers, hashtags, mentions |
| | Count | `core.count` | Characters, words, lines, list entries |
| Numbers | Calculate | `core.math` | + − × ÷, remainder, power, min, max, square root, absolute value |
| | Round number | `core.round` | Decimal places (negative rounds to tens, hundreds), nearest, up or down |
| | Format number | `core.formatNumber` | Fixed decimals and thousands separators, as text |
| | Random number | `core.random` | Between two values, whole or not |
| Lists | Calculate statistics | `core.statistics` | Count, sum, average, median, min, max, range, sample standard deviation over all items (lists are flattened, text that is a number counts) |
| | Combine items | `core.combineItems` | All items, or one value from each, as one list in one item |
| | Sort items | `core.sort` | By a field (numbers before text, missing values last), descending, reversed or shuffled |
| | Remove duplicates | `core.dedupe` | Keep the first item for each field value or whole item |
| | Get items | `core.pick` | First, last, item number (negative counts from the end), a range, or a random item |
| | JSON | `core.json` | Read JSON text into fields, or write a value as JSON text |
| | Get URL parts | `core.url` | scheme, host, port, path, query (decoded, last value wins), fragment, origin |
| Dates | Format date | `core.dateFormat` | ISO 8601, date, time, long, Unix seconds or ms, or a pattern |
| | Adjust date | `core.dateAdjust` | Add or subtract seconds to years, or go to the start or end of a unit |
| | Time between dates | `core.dateDiff` | Whole seconds to years between two dates |

If and Filter also gained **matches** and **does not match** a regular
expression.

## Rules that keep the three engines identical

The server, iPhone (JavaScriptCore) and Android (Kotlin) engines must give
the same answers ([native/WORKFLOWS.md](../native/WORKFLOWS.md)), so the steps
avoid anything a platform library would do differently:

- **Regular expressions** use JavaScript syntax everywhere. The server
  translates them for Python `re` (`workflow_script._translate_regex`):
  `\d`, `\w`, `\b` are ASCII, `\s` is JavaScript's whitespace, `.` stops at
  line ends, `$` means the end of the text. Android translates the same way
  for `java.util.regex`.
- **Words and spaces** are ASCII whitespace; characters are Unicode code
  points, so an emoji counts once.
- **Rounding** works on the shortest decimal form of the number, half away
  from zero, so 2.675 rounds to 2.68 as a person expects.
- **Remainder** follows Python: the result has the sign of the divisor
  (−7 remainder 3 is 2).
- **Dates** are read as ISO 8601 (`2026-10-17`, `2026-10-17 09:30`,
  `...T09:30:00Z`, `+02:00`), or as Unix seconds (milliseconds from 10^11 on).
  A date without an offset is in the step's time zone. Time zones are `UTC`,
  a fixed `+05:30`, or an IANA name; a local time inside a clock change uses
  the offset from before the change. Dates run from year 1 to 9999. Patterns
  use `YYYY YY MMMM MMM MM M DD D dddd ddd HH H hh h mm m ss s SSS A a Z ZZ
  X x`, with `[text]` kept as is; names are English.
- **Random numbers** come from the engine's random source, so tests can feed
  fixed values (`randoms` in the conformance cases).
- **Run workflow** shares the parent's request budget and deadline and nests
  at most two levels deep (`MAX_WORKFLOW_DEPTH` 3 counts the top run). Its
  output is the items of the workflow's last steps, or of its Stop step. On
  the server the runtime looks the workflow up among the owner's own
  (`workflows_runtime._workflow_loader`); a phone fetches the workflows a run
  can reach before it starts and passes them in.

## n8n files

Import maps n8n Code (JavaScript, with a warning that Python code cannot
run), Wait (capped at 60 seconds, with a warning), Remove Duplicates, Sort and
Aggregate to these steps; export writes Code, Wait, Remove Duplicates and
Sort as their n8n nodes. Every other step exports as `forkmesh.core.*` with
`forkmeshType`/`forkmeshParams`, which round-trips exactly.
