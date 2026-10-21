# Automated AI review of pull requests

An instance can have Cloudflare Workers AI read each pull request and post a
first-pass review as one ForkBot comment. A maintainer still decides. The
review never approves, never requests changes, never merges and never blocks
a merge.

Code: `app/src/ai_review.py` (queue, provider call, comment),
`app/src/review_prompt.py` (the shared review prompt),
`app/public/dashboard/js/09-ai-review.js` (settings, status chip, comment
rendering), migration `0281_pull_ai_reviews`. Tests:
`app/tests/test_ai_review.py`, `app/tests/test_ai_review_frontend.py`.

## Turning it on

Administrators: **Settings → AI review**.

| Setting | Default | Notes |
| --- | --- | --- |
| Review pull requests automatically | off | Nothing is queued or sent while off. |
| On for every public repository | off | Off: repositories opt in. On: repositories opt out. |
| Cloudflare account ID | none | 32 hex characters. |
| API token | none | Sealed with `encrypt_row` in `instance_settings` (`ai_review`). The page shows only whether one is set. |
| Model | `@cf/openai/gpt-oss-120b` | Models are limited to an approved list (`APPROVED_AI_REVIEW_MODELS`); anything else is refused. |
| Reviews per day (instance) | 50 | Provider calls per UTC day. |
| Largest diff to review | 512 KB | Bigger pull requests get a one-time note instead. |
| Reviews per repository per day | 20 | |
| Reviews per pull request author per day | 10 | A maintainer's "Run AI review" is not counted against the author. |
| Use this server's AI binding | off | Only shown when the deployment has an `AI` binding; used when no token is set. |

Create the token in the Cloudflare dashboard with only the **Workers AI**
permission (Read and Edit) on the one account. The instance never needs any
other Cloudflare permission for this.

Repositories decide in `.forkmesh/pulls.json` on the default branch:

```json
{"aiReview": true}
```

`true` opts in, `false` opts out, anything else follows the instance default.
The same file's `reviewPromptTemplate` (see below) changes the instructions.
The file is read from the default branch, never from the pull request, so a
pull request cannot turn the review on, off or rewrite its instructions.

## Approved models

`APPROVED_AI_REVIEW_MODELS` in `app/src/ai_review.py`, each with the most
prompt characters it is given:

| Model | Context |
| --- | --- |
| `@cf/openai/gpt-oss-120b` (default) | 128k tokens |
| `@cf/meta/llama-4-scout-17b-16e-instruct` | 131k tokens |
| `@cf/mistralai/mistral-small-3.1-24b-instruct` | 128k tokens |
| `@cf/google/gemma-4-26b-a4b-it` | 256k tokens |
| `@cf/openai/gpt-oss-20b` | 128k tokens |
| `@cf/meta/llama-3.3-70b-instruct-fp8-fast` | 24k tokens: the diff budget shrinks to fit |

The list was checked against the Workers AI catalog
(`GET /accounts/{id}/ai/models/search` and `/ai/models/schema`, read-only):
every listed model accepts `messages` and `max_tokens` on `/ai/run`. A model
missing from the list cannot be saved and is never sent to the provider, so
the instance's budget cannot be aimed at other models. Changing the list is a
code change with a test (`test_default_model_is_approved_and_others_are_refused`).

## When a review runs

| Trigger | Where |
| --- | --- |
| A pull request is submitted through the relay | `pulls_handler` (`ai_review_enqueue`, after the inbox insert) |
| A maintainer presses **Update branch** | `repository_pull_merge_handler`, `mutation == "update"` |
| A push moves the head branch of a branch-backed pull already in the queue | `core_push.record_accepted_push` (`ai_review_note_push`) |
| A maintainer presses **Run AI review** | `POST /api/ai-review/pulls/<owner>/<repo>/<n>` |

Enqueuing writes one `pull_ai_reviews` row per pull request (keyed by the
repository's blind index and the number). The cron phase `ai-review` (every
five minutes, at minutes divisible by five) takes at most three due rows and makes at most one
provider call per run, so a run stays inside the phase's subrequest budget
(`_cron_subrequest_charge` counts every fetch and D1 call). The provider call
has a 25 second timeout. A review is never run inside a request.

It is not a workflow: a workflow for an untrusted pull request runs without
secrets, and the token must never be reachable from code a contributor
controls. The job only reads text and writes a comment.

Pull requests opened directly on a node (agent sessions, imports) never pass
through the relay and are not queued; **Run AI review** covers them.

## What the job does

1. Skips (with a reason shown in the chip) when the repository is private,
   not opted in, the pull request is closed, merged, deleted or a draft
   (`draft: true`, or a title starting with `Draft:`, `WIP:`, `[Draft]`,
   `[WIP]`).
2. Reads the record from `forkmesh/pull/<n>` (then `forkmesh/pulls`) through
   the public gateway: `pull.md`, `changes.patch`, `commits.mbox`. A
   branch-backed pull (`derive: branch`) reads `compare?base=&head=` instead.
   A record not written yet is retried every minute, at most 30 times.
3. Works out the head: `headOid`, the compared head commit, or a hash of the
   patch. **Dedupe:** a head equal to `reviewed_head` is not reviewed again.
4. Builds the input: drops binary files, lockfiles (`package-lock.json`,
   `yarn.lock`, `go.sum`, `Cargo.lock`, …) and generated files (`dist/`,
   `vendor/`, `*.min.js`, source maps, very long lines), cuts each file's
   diff at 16 KB and the whole diff at 96 KB (less for a small-context
   model), and lists what was left out. A diff above the instance limit gets
   one "too large to review automatically" comment per pull request and no
   provider call.
5. Checks the budgets in `ai_review_budget`: the instance cap defers the job
   to the next UTC day; the repository and author limits skip it.
6. Calls `POST https://api.cloudflare.com/client/v4/accounts/{account}/ai/run/{model}`
   with `Authorization: Bearer <token>` and
   `{"messages": [system, user], "max_tokens": 2048, "temperature": 0.2}`.
7. Posts the comment, records `reviewed_head`, the model and the suggested
   verdict.

Failures (HTTP errors, timeouts, empty answers, a full inbox) are retried
with backoff (2, 4, 8 minutes) up to four attempts, then the chip says
**Failed**. `log_error` gets only a code such as `http_429 (attempt 2)`:
never the token, the prompt, the diff or the answer.

## The prompt and prompt injection

The system message holds the rules and the review instructions; the user
message holds the pull request.

- Everything from the pull request (title, author, branch names,
  description, commit messages, file names, diff) is in the user message
  between `<<<UNTRUSTED_PULL_REQUEST_<nonce>>>>` and
  `<<<END_UNTRUSTED_PULL_REQUEST_<nonce>>>`, with a fresh random nonce per
  call, so the author cannot close the block. Marker-like text in the data
  is replaced.
- The system message says that the block is data, that instructions inside
  it are to be ignored and reported under Security, that the model has no
  tools and cannot approve or merge, and fixes the output headings.
- The review instructions are the same robust review prompt as the pull
  page's "Review with your agent" box (`review_prompt.DEFAULT_TEMPLATE`), or
  the repository's `reviewPromptTemplate`. Placeholders the pull request's
  author controls (`{title}`, `{author}`, `{base}`, `{head}`) are filled with
  "see the untrusted data" in the system message; only forge-made values
  (`{url}`, `{number}`, `{repo}`, `{host}`, `{cloneUrl}`, `{diffUrl}`,
  `{filesUrl}`) are inlined.
- The model gets no tools, no repository access and no network. Nothing it
  says is executed; its only effect is the text of one comment.

`review_prompt.py` is the shared boundary for the prompt text. The pull page
box keeps its own copy for the server-rendered first view
(`app/host/forkmesh_host/pull_review_prompt.py`, when present);
`test_review_prompt_matches_the_pull_page_box` fails when the two differ.

## The comment

Author `forkbot`, name `ForkBot`, an unsigned `comment` event through the
pull inbox, like app comments (`apps_runtime.pull_comment`). Body:

```
### 🤖 Automated AI review — Cloudflare Workers AI (`<model>`) · head `<short sha>`

> Automated; may be wrong; a maintainer decides. This review never approves, requests changes or merges.

### Summary … ### Suggested verdict … ### Blockers … ### Security …
### Correctness … ### Tests … ### Nits … ### Questions …

_Reviewed 7 of 9 files (41 KB of diff); not reviewed: 1 lockfile, 1 generated. Only the pull request's text was sent to the model._
```

It is a `comment`, never a `review`, so `pullReviewPolicy` and the merge
executor never count it as an approval or a request for changes.

The model's answer is untrusted Markdown. `sanitize_report` removes raw HTML
and comments, turns images into their alt text and links into their text,
wraps bare URLs and `@mentions` in code spans (no link, no ping), lowers `#`
and `##` headings to `###`, removes control and invisible characters, closes
an open code fence and cuts the answer at 14,000 characters. The dashboard
then renders it with the same escaping renderer as every comment
(`renderRecordMarkdown`).

The ledger is append-only, so a new head gets a new comment. The pull page
shows the newest AI review in full (Summary and Suggested verdict open, the
rest behind **Show the full report**) and folds every earlier one into a
single "Earlier automated AI review of head … · superseded" line.

## Pull page

The sidebar has an **AI review** block when the instance has it on: a chip
(Queued, Reviewing, Done, Skipped, Failed) with the reason and head, and a
**Run AI review** button for the repository owner and organization owners or
admins (`_authorize_issue_manager`). The chip polls every 20 seconds while
queued or reviewing. It works at 375 px and in both themes (theme tokens
only).

`GET /api/ai-review/pulls/<owner>/<repo>/<n>` answers
`{enabled, status, reason, head, reviewedHead, model, verdict, canRun}`;
`POST` with a session queues a run (maintainers, opted-in repositories,
at most "reviews per repository per day" presses per maintainer per day).

## Privacy

Only the pull request's text is sent to Cloudflare Workers AI, under the
instance's own Cloudflare account: title, description, author name, branch
names, commit messages, the list of changed files and the (trimmed) unified
diff. Nothing else: no other repository files, no comments, no account data,
no email addresses, no tokens. Private repositories are never sent. Cloudflare
processes it under the account's terms for Workers AI.

## Storage

- `instance_settings` row `ai_review`: settings; the token sealed with
  `encrypt_row`. Never returned by any endpoint.
- `pull_ai_reviews`: status, reason, head, reviewed head, attempts, next run,
  `head_bi` (blind index of `owner/repo:branch` for the push trigger) and an
  encrypted `data` (owner, repository, model, verdict, requester).
- `ai_review_budget`: per-day counters keyed `instance`, `repo:<bi>`,
  `author:<bi>`, `click:<bi>`.

See [REKEY.md](../REKEY.md) for how a key rotation treats them.
