# OTF feedback survey

The Open Technology Fund reviewers asked who needs ForkMesh, why existing
approaches fall short, whether it survives censorship, whether developers can
trust and run it, and who has tested it. This survey asks internet freedom
developers and maintainers those questions, so the answers become evidence
for the proposal (OTF priorities 1, 2, 5, 6, 7, 8 and 9 on the Tasks board, from
the **OTF Feedback** note). It is the written half of the outreach; the
8 to 12 interviews use the same questions.

[`otf-survey.json`](otf-survey.json) is the survey, written as the body of a
[Surveys](../../marketplace/surveys.md) create request: 27 questions, about 10
minutes. Only the first question is required.

## Send it

With the Surveys plugin installed in the organization, create it as a draft
from a shell with a session token, then open it from the dashboard:

```sh
curl -X POST "https://app.forkmesh.com/api/surveys/$ORG/items" \
  -H "Authorization: Bearer $FORKMESH_SESSION" -H 'content-type: application/json' \
  --data @docs/finance/grants/otf-survey.json
```

Then in **Surveys** open the draft, check it with **Preview**, press
**Open**, and copy its link (`/survey?t=…`, no account needed to answer).
Post it to net4people and ntc.party, and send it with each interview
request. Close it on Fri Nov 13 so the research summary is ready for
Mon Nov 16; export the CSV for the report.

## Safety

Respondents may be in countries that punish this work. The survey asks for
regions, not countries, never requires a name, project or contact, and tells
people to leave identifying details out of free text. Responses are sealed and
visits are counted with a blinded key ([Activity](../../marketplace/surveys.md#activity)).
Publish only anonymized summaries, and keep the CSV off shared drives. See the
[threat model](../../protocol/threat-model.md) and
[censorship resilience](../../censorship-resilience.md).

## What each question answers

| Questions | Answers | OTF priority |
| --- | --- | --- |
| 1-4: role, team size, user regions (UN M49), where code lives | Who needs it, and where | 1, 10 |
| 5-7: incidents, the worst one, how serious | The real problem, with stories | 1 |
| 8-10: current workarounds, their cost, how users find a genuine copy | Why existing approaches fall short | 1, 3, 7 |
| 11-12: most wanted features, keeping the current host | Requirements, coexistence with GitHub and GitLab | 3, 7 |
| 13-15: threats, what earns trust, what breaks it | Threat model input and review needs | 2, 4 |
| 16-18: running a mirror, budget, what setup needs | Whether small teams can self-host | 5 |
| 19-23: tried it, ease, reliability, problems, intent to use | Prototype feedback | 8 |
| 24: help testing from a censored network | Test environments and partners | 6 |
| 25-27: interview, pilot, mirror host, reviewer, letter of support, contact | Buy-in, partners and letters | 1, 9 |

Question ids are fixed (`0f000001` to `0f00001b`, in order), so results and
the CSV keep their columns if the survey is edited after it opens.
