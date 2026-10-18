# Scripts in workflows (Run script)

Part of [the App docs](README.md); the other steps are in
[workflow-steps.md](workflow-steps.md). The **Run script** step
(`core.code`) runs JavaScript the user writes, for whatever the ready-made
steps do not cover. It is interpreted by ForkMesh's own sandboxed interpreter
(`app/src/workflow_script.py`), never by `eval`, a JavaScript engine's
`Function`, or a Python `exec`: a script can only compute with the values it
is given. It cannot open files, call the network, import anything, or reach
the host's objects, and it is stopped after a fixed number of steps.

## What a script sees and returns

| Name | Meaning |
| --- | --- |
| `items` | The incoming items as plain objects (copies; changing them does not touch earlier steps) |
| `$input.all()`, `.first()`, `.last()`, `.item` | n8n-style `{json}` wrappers around the same objects |
| `$json`, `item`, `$index` | The current item and its position, in "Once for each item" mode |
| `$trigger`, `$node["Step"].json`, `$now`, `$today`, `$workflow` | As in expressions |
| `$vars` | Workflow variables; changes are kept for later steps when the script succeeds |
| `$utils` | `hash(text, algorithm, encoding)`, `hmac(text, key, algorithm, encoding)`, `base64Encode`, `base64Decode`, `urlEncode`, `urlDecode`, `formatDate(date, formatOrPattern, timeZone)`, `formatNumber(number, decimals, grouping)`, the same code as the steps |
| `console.log` (`info`, `warn`, `error`) | Lines shown under the step's output (100 lines, 1,000 characters each) |

Once for all items: the returned array becomes the step's items; an object
becomes one item; `null` gives none; returning nothing passes `items` on as
the script left them. Once for each item: the same per item, and returning
nothing passes the (possibly changed) item. Entries shaped like n8n's
`{json: {...}}` are unwrapped; a value that is not an object becomes
`{"value": ...}`. Numbers that are whole come out as integers, dates as ISO
strings, and functions are dropped. The default code in a new step shows the
pattern.

## The language

A large part of modern JavaScript: `let`/`const`/`var`, functions and arrow
functions (defaults, rest parameters, closures, recursion), destructuring,
spread, template literals, optional chaining, `??`, every operator including
`**` and bitwise ones, `if`, `for`, `for...of`, `for...in`, `while`,
`do...while`, `switch`, `break`/`continue`, `try`/`catch`/`finally`,
`throw`, regular expression literals, `this` in object methods, and
automatic semicolons. Built in: `Math`, `JSON`, `Object`, `Array`, `String`,
`Number`, `Boolean`, `Date`, `RegExp`, `Set`, `Map`, the error types,
`parseInt`, `parseFloat`, `isNaN`, `isFinite`, `encodeURIComponent`,
`decodeURIComponent`, with the usual string, array, number, date and regular
expression methods.

Left out on purpose: `class`, `async`/`await`, generators, `import`,
`eval`, `Function(...)`, timers, `fetch`, getters and setters, labels,
`Symbol`, typed arrays. Those raise "... is not supported in scripts" or
"... is not defined".

Differences from a browser, chosen so the three engines agree:

- Strings count Unicode code points: `'😀'.length` is 1 and `charCodeAt`
  returns the code point.
- There is one time zone, UTC. `new Date()` and `Date.now()` are the time the
  run started, so a run is repeatable.
- Error objects are plain `{name, message}` objects, so they show up when
  returned or logged.
- `keys()`, `values()` and `entries()` return arrays.

`app/tests/fixtures/workflow_script_corpus.json` holds programs with the
results Node gives; the server interpreter must match them
(`app/tests/test_workflow_script.py`), and the phone engines replay them as
conformance cases.

## Limits

| Limit | Value |
| --- | --- |
| Steps (statements, loop rounds, calls) per step run | 100,000 |
| Call depth | 40 |
| Nesting of brackets and blocks | 50 |
| String length | 1,000,000 characters |
| Array, Set or Map size | 100,000 |
| Script length | 20,000 characters (`MAX_VALUE_CHARS`) |
| Regular expression matching, all steps of one run together (server) | 2 s |

Python's `re` cannot be interrupted from inside, and a pattern such as
`(a+)+$` backtracks for minutes, so on the server every match of a user's
pattern (scripts, Match text, Replace text, If "matches") runs under a
`SIGALRM` timer for what is left of the run's budget
(`workflow_script.GuardedPattern`; `_sre` checks for signals while it
backtracks). The timer works on the event loop's main thread, where runs
happen; elsewhere matching runs without it. Phones skip the budget: a phone
run only spends the phone's own time.

The step limit, the regular expression budget and the run deadline cannot be
caught with `try`; string and
array size errors are ordinary `RangeError`s. Errors report the line:
`Line 3: TypeError: Cannot read properties of null (reading 'x')`.

The script field is never treated as an expression: `{{ ... }}` inside the
code stays as written.
