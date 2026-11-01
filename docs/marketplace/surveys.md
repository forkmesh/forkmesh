# Surveys

An organization plugin for running surveys: members build a survey from
questions, publish it, share its private link, watch responses arrive, read
the results as counts, averages and answers, and export them as CSV. The owner
asked for a survey plugin where surveys can be run and managed on the desktop,
the web and phones, so every app offers the same actions: create, edit,
duplicate, open, close, reopen, delete, read results and responses, delete a
response, copy or share the link, export, and install.

Package: [`marketplace/packages/surveys`](../../marketplace/packages/surveys/README.md).
Members manage surveys on the web (`/dashboard/surveys`, laid out for phones
too), in a native desktop page, and in native iPhone and Android screens.
People answer on a public page at `/survey?t=<token>` with no account.

## Code map

| Path | What it does |
| --- | --- |
| `content/worker/surveys.py` | The Worker API at `/api/surveys`: surveys, questions, publishing, the public read and submit routes, results, responses, CSV export, org export and purge. |
| `content/web/view.html`, `content/web/page.js` | The dashboard page: totals, the survey list, the question editor, the detail sheet with results and responses. |
| `content/www/survey.{html,js,css}` | The respondent's page at `/survey?t=<token>`. Built with no site header, analytics or `innerHTML`. |
| `content/desktop/SurveysPage.{h,cpp}` | The native desktop page and its registration (`/api/surveys`, proof `forkmesh-surveys-request-v1`). |
| `content/desktop/SurveysModel.{h,cpp}`, `content/desktop/tests/test_surveys_model.cpp` | The page's logic without widgets (labels, filters, allowed actions, question validation, result lines) and its Qt test, target `forkmesh-surveys-tests`. |
| `native/ios/ForkMeshCore/Sources/FMPackages/Survey*.swift`, `PackagesClient+Surveys.swift`; `native/ios/ForkMesh/Features/Packages/Survey*.swift` | iPhone models, client, screen model and screens. Tests: `FMPackagesTests/SurveysTests.swift`, `FMApiTests/SurveyRouteTests.swift`. |
| `native/android/core/.../market/Surveys.kt`, `native/android/app/.../feature/market/SurveyScreens.kt` | Android models, client, screen state and screens. Test: `core/src/test/.../market/SurveysTest.kt`. |
| `tests/test_surveys.py` | API, public submit, limits, results, export and lifecycle contracts against sqlite. |

## Questions

A survey holds up to 50 questions. Each question is an object:

| Field | Meaning |
| --- | --- |
| `id` | 8 lowercase hex characters, kept across edits so results stay attached. A missing or malformed id is replaced when saving. |
| `type` | `single` (pick one), `multiple` (pick any), `text` (one line), `paragraph` (long text), `rating` (1 to `scale`). |
| `title` | Up to 300 characters; a question without a title is dropped when saving. |
| `help` | Optional text under the title, up to 1000 characters. |
| `required` | Whether a respondent must answer it. |
| `options` | For `single` and `multiple`: 2 to 30 distinct labels of up to 200 characters. Blank and repeated labels are dropped; fewer than 2 left is `invalid_question_options`. Empty for the other types. |
| `scale` | For `rating`: 3 to 10, default 5. 0 for the other types. |

Answers are keyed by question id. A `single` answer is one of the option
labels, `multiple` a list of them, `text` one line (cut at 500 characters),
`paragraph` text (cut at 5000), `rating` an integer from 1 to `scale`.
Anything else is `invalid_answer` with the question id in `question`; a
required question left empty is `answer_required` with the question id, and a
submission with no answer at all is `empty_response`. Answers to ids the
survey does not have are ignored.

Saving a survey reports a bad question as `invalid_question_options`,
`invalid_question_type` or `too_many_questions` with its position (from 0) in
`question`.

Questions can be edited while a survey is open or after it has responses.
Results are counted by question id and only from answers that still fit the
question (an option label that was renamed no longer counts), so changing a
question's type or options changes what its results show, never the stored
responses.

## Status

`status` is `draft`, `open` or `closed`. A survey opens with at least one
question (`survey_has_no_questions` otherwise). `displayStatus` adds
`scheduled` for an open survey whose `opensAt` is still ahead, and reads
`closed` for an open survey past `closesAt` or with `maxResponses` reached. The
public page accepts answers only while `displayStatus` is `open`.

Opening or reopening refuses a survey whose `closesAt` has passed
(`survey_close_date_passed`) or whose `maxResponses` is already reached
(`survey_response_limit_reached`), both 409, so the button never leaves a
survey that looks open but takes no answers. Closing a draft is
`survey_not_open`. An open or closed survey cannot be saved with no
questions.

## Settings per survey

`settings` holds `oneResponsePerPerson` (default `false`), `opensAt` and
`closesAt` (milliseconds, 0 for none), `maxResponses` (0 for no limit, at
most 10,000), `notifyOnResponse` (default `true`: the creator gets a Ping,
at most one per survey per hour) and `shuffleOptions` (default `false`: the
public page shows choice options in random order).

Each response records a respondent key: the first 32 hex characters of the
instance's blind of the survey id and the respondent's connection address.
With `oneResponsePerPerson`, a second submission with the same key is
`already_responded` (409); the public page also remembers the survey in the
browser. Any key may submit at most 10 responses per survey per hour
(`too_many_responses`, 429). An instance in privacy mode never sees the
address, so the key is empty and neither check applies.

## API

All routes answer JSON with `ok`. Organization routes need a session (Bearer
token on the web and phones, the signed `forkmesh-surveys-request-v1` request
on the desktop) and membership of the organization; `plugin_not_installed`
(409, with `canInstall`) means the organization has not installed Surveys.

| Route | Method | Body / answer |
| --- | --- | --- |
| `/api/surveys` | GET | `{actor, organizations: [{name, role, installed}]}` |
| `/api/surveys/<org>` | GET | `{organization, role, canManagePlugin, surveys: [Survey without questions], summary}` |
| `/api/surveys/<org>/items` | POST | `{title, description, thankYou, questions, settings}` → `{survey}` (201, a draft) |
| `/api/surveys/<org>/items/<id>` | GET | `{survey}` with `questions` and `results` |
| `/api/surveys/<org>/items/<id>` | PATCH | any of the create fields → `{survey}` |
| `/api/surveys/<org>/items/<id>` | DELETE | `{deleted: id}`; the creator or an owner or admin only (`survey_delete_forbidden`, 403) |
| `/api/surveys/<org>/items/<id>/open` | POST | publishes or reopens → `{survey}` |
| `/api/surveys/<org>/items/<id>/close` | POST | → `{survey}` |
| `/api/surveys/<org>/items/<id>/duplicate` | POST | a draft copy titled "Copy of …" with no responses → `{survey}` (201) |
| `/api/surveys/<org>/items/<id>/responses` | GET | `{responses: [{id, submittedAt, answers}]}`, newest first, at most 2,000 |
| `/api/surveys/<org>/items/<id>/responses/<rid>` | DELETE | `{deleted: rid, survey}` |
| `/api/surveys/<org>/items/<id>/export` | GET | `{filename, csv}`: one row per response, one column per question, `multiple` answers joined with `; ` |
| `/api/surveys/-/public/<token>` | GET | `{survey: {title, description, questions, status, thankYou, oneResponsePerPerson, shuffleOptions, organization}}`; 404 `survey_not_found` for a draft, a bad token or an uninstalled plugin |
| `/api/surveys/-/public/<token>` | POST | `{answers}` → `{thankYou}` (201); `survey_closed` (409) when not open |

Install, uninstall and export of all of an organization's surveys go through
the shared plugin routes, `/api/plugins/<org>/surveys/{install,export,uninstall}`.

A **Survey** in a list or detail:

```json
{
  "id": "32 hex", "organization": "acme", "title": "Team offsite feedback",
  "description": "", "thankYou": "Thanks for answering.",
  "status": "open", "displayStatus": "open",
  "settings": {"oneResponsePerPerson": false, "opensAt": 0, "closesAt": 0,
               "maxResponses": 0, "notifyOnResponse": true, "shuffleOptions": false},
  "questionCount": 4, "responseCount": 12, "lastResponseAt": 2000000000000,
  "publishedAt": 2000000000000, "closedAt": 0,
  "createdBy": "alice", "updatedBy": "alice", "createdAt": 0, "updatedAt": 0,
  "canDelete": true, "publicUrl": "https://app.forkmesh.com/survey?t=<id>.<24 hex>"
}
```

`publicUrl` is empty for a draft. The detail adds `questions` and `results`,
one entry per question in order:

```json
{"id": "a1b2c3d4", "type": "single", "title": "How was it?", "answered": 10,
 "skipped": 2,
 "options": [{"label": "Great", "count": 7}, {"label": "Fine", "count": 3}],
 "average": 0, "distribution": [], "answers": []}
```

`options` is filled for `single` and `multiple` (counts can add up to more
than `answered` for `multiple`); `average` (one decimal) and `distribution`
(`scale` counts, for 1 up to `scale`) for `rating`; `answers` (the 20 newest
non-empty texts) for `text` and `paragraph`.

Results are worked out from the 2,000 newest responses on each read, since
every response is sealed and has to be opened; `resultsCounted` says how many
were counted, and the apps say so when it is less than `responseCount`. The
CSV export always holds every response. Cells that start with `=`, `+`, `-`
or `@` get a leading `'` so a spreadsheet does not run an answer as a
formula.

The overview `summary` is `{surveys, open, drafts, closed, responses,
responsesLast7Days}`.

## Data

Two tables, created when an organization installs the plugin and dropped
when the last install is removed:

- `survey_forms`: one row per survey. Status, counters and dates are plain
  columns; the title, description, thank-you text, questions and settings are
  sealed with the instance row key. 500 surveys per organization.
- `survey_responses`: one row per response, answers sealed, with the
  respondent key and submit time plain for the limits above. 10,000 responses
  per survey; past that the public page reports the survey as closed.

The submit route is public and accepts a JSON `POST` only from an allowed
origin (`same_origin`), as the other plugin writes do, so another site cannot
post answers from a visitor's browser.

`response_count` on the survey is kept in step by the Worker on every submit
and delete rather than counted on each read, so the list stays one query.

## Apps

**Web.** The list becomes cards below the `sm` breakpoint. The editor is a
modal dialog with one card per question (type, title, help, required, options
or scale, move up and down, remove). The detail dialog has Results and
Responses tabs, the share link with Copy, and the actions the survey's status
allows. Messages go to the open dialog's own error slot, as Invoicing does.
`/dashboard/surveys?org=<org>&survey=<id>` opens a survey, which Pings use.

**Desktop.** A native page in the rail with the number of open surveys as its
badge, search hits on title, and `exportData()`. In Control Tower the list and
the open survey sit side by side; in Focus a survey opens as its own page with
a back button. "Open on the web" opens the dashboard page.

**iPhone and Android.** A Business hub entry that appears once one of the
user's organizations installs Surveys. The route is
`surveys(organization, survey)` on both and the dashboard links above open
it. Both apps can create and edit surveys with every question type, open and
close them, share the link with the system share sheet, read results and
responses, delete responses and export the CSV through the share sheet.
