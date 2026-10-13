# Managed-services pricing interview register

This register is the evidence gate for the managed-services plan in
`www/public/business-model.json`. It contains no customer research as of
2026-08-20.

## Current decision

- Validation state: **not validated**
- Qualifying decision-maker interviews: **0**
- Managed checkout: **disabled**
- Price shown publicly: **none**
- Next decision: retain the research preview until the minimum cohort passes

No issue comment, contributor offer, analytics event, support ticket,
competitor price, internal opinion, or synthetic response is a pricing
interview. This explicit zero is preferable to inventing demand.

## Hypotheses to test

These are hypotheses, not findings:

| Field | Initial hypothesis |
| --- | --- |
| Economic buyer | Engineering manager, platform lead, or maintainer who owns hosting/reliability toil |
| Budget owner | Engineering infrastructure, developer productivity, or support/operations budget |
| Candidate budget | Must be elicited; no band has been validated |
| Paid job 1 | Operate reliable mirrors and recovery without maintainer toil |
| Paid job 2 | Administer a team, policy, identity, and audit boundary |
| Paid job 3 | Provide accountable migration, incident, and priority support |
| Secondary jobs | Managed indexing/discovery and bounded hosted runners |

## Minimum validation rule

Run at least five qualifying interviews with people who control or directly
recommend a relevant budget, spanning at least three organizations and two
organization-size bands. A result may move to `validated` only when:

1. at least three respondents name the same buyer role and an existing budget
   owner or purchasing path;
2. at least three provide a credible current spend, approved range, or
   budget-comparison anchor rather than a hypothetical willingness-to-pay;
3. each of the published top three paid jobs is independently selected in at
   least three interviews, or the public ordering is revised to match the
   observed ranking; and
4. every contradictory result and every resulting plan, unit, allowance, price,
   support, or positioning revision is logged below.

The facilitator must not pitch a price before learning the current process,
failure cost, buyer, and budget. Interest without authority is demand context,
not buyer validation.

## Interview record template

Copy one record per real interview. Store detailed notes only in the consented
research location and link them; do not commit names, emails, private project
names, recordings, or confidential budgets to this public repository.

```text
Interview id: PI-YYYY-NNN
Date (UTC):
Facilitator:
Consented evidence link:
Respondent role:
Organization-size band: 1-10 / 11-50 / 51-250 / 251+
Decision authority: buyer / recommender / user-only
Current workaround and spend anchor:
Budget owner and purchasing path:
Highest-cost failure or toil:
Top paid jobs, ranked:
Acceptable unit and why:
Budget range or comparison anchor:
Objections / portability concern:
Would enter bounded trial now: yes / no, with condition
Public-contract revision required:
Qualified: yes / no, with reason
```

## Revision log

| Version | Date | Evidence | Revision | Validation effect |
| --- | --- | --- | --- | --- |
| 0 | Before 2026-08-20 | Legacy marketing and membership implementation | Mixed free software, $5 Supporter and $10 Pro memberships, hosted workflows, and custom enterprise copy without a portable entitlement matrix or cost gate. | Not validated |
| 1 | 2026-08-20 | Product contract #476 plus repository capability, privacy, billing, and infrastructure-usage audits; **not customer interviews** | Separated permanent protocol rights, Hosted Free, Pro membership, and managed-service hypotheses; named observable usage units; prohibited unlimited variable-cost plans; disabled managed checkout pending evidence. | Still not validated; buyer and budget remain hypotheses |

Future rows must cite the interview ids that caused the revision. A revision
with no customer evidence may improve clarity or safety but cannot advance the
validation state. When the minimum cohort qualifies, add only its non-identifying
`PI-YYYY-NNN` ids to `pricing-interviews.json`, record a new revision row, and
set `validatedRevision` to that row's version. Counts without those ids and a
new documented revision fail the automated contract.
