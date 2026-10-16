# Grants

The 100 grants, funds and credit programs that best fit ForkMesh, ranked, with
the amount, deadline, who can apply, the application steps, every document the
application asks for, and a one-click ChatGPT draft for each. Researched on
2026-10-16 against each funder's own pages.

| File | What it is |
| --- | --- |
| [ranked.md](ranked.md) | The ranked list: a summary table, then one section per grant |
| [forkmesh-grants.csv](forkmesh-grants.csv) | The same list as a CRM import for the **Grants** pipeline |
| `grants.json` | The source both are built from (`tools/grants_build.py`) |

## Apply in three clicks

1. Open [ranked.md](ranked.md) and pick the next grant from the top.
2. Click **Draft this application in ChatGPT**. ChatGPT opens with a prompt
   that holds the ForkMesh brief, the funder's criteria, amount, deadline,
   steps and required materials, and asks for the full application in the
   funder's own questions and limits, then a checklist of what only we can
   supply (entity details, bank details, CVs, letters).
3. Check the draft against the live form (**Apply**), fill the checklist, and
   submit. Move the grant to **submitted** in the CRM.

## The Grants pipeline in the CRM

The CRM has a **Grants** pipeline type (`grants`) with the stages
`researching`, `eligible`, `drafting`, `submitted`, `awarded` and `declined`
(`marketplace/packages/crm/README.md`). New organizations get it as a starter
pipeline.

To put this list into the organization's CRM, an owner or admin opens
**CRM → Import CSV** and imports `forkmesh-grants.csv`. Every row names the
`grants` pipeline, so the import adds the Grants pipeline if the organization
does not have it yet and opens a **forkmesh grants** list bound to it (rename
it from its tab menu). Each grant is a row: **Name** is the program, **Company** the funder, **Deal value**
the top of the award in US dollars, **Next step** the deadline, **Notes** the
full application details; **Rank**, **Fit score**, **Award**, **Deadline**,
**Round**, **Geography**, **Who can apply**, **Effort**, **Decision time** and
**Apply at** are their own columns.

On a contact in a Grants-type pipeline, **Write with AI** opens on
**Grant application**, so the CRM row is also a one-click ChatGPT or Claude
draft built from the row's notes.

Rows have no email address, so the CRM cannot match them on a second import:
import the file once, then work in the CRM. To bind an existing list instead,
use **Pipelines and stages → Lists** and pick Grants for it.

## How the ranking works

Each grant has a fit score out of 100 that weighs mission fit (open-source
infrastructure, supply-chain integrity, resilience and censorship resistance,
preservation, decentralized storage and compute), how likely ForkMesh is to be
eligible, the size of the award, and the effort to apply. Ties go to grants
whose program page was confirmed live, then to the larger award.

Eligibility often depends on the legal entity (country; for-profit or
nonprofit; fiscal sponsor). Check **Who can apply** and **Geography** before
drafting; government programs in particular are limited to entities registered
in their country or region. Grants marked "no, confirm first" under
**Checked live** could not be confirmed from an official page on the research
date. Deadlines move: the prompt asks ChatGPT to read the live page first.

## Updating the list

Edit `grants.json` (one object per grant; `tools/grants_build.py` lists the
keys) and run `python3 tools/grants_build.py`. `--check` fails when the CSV or
the ranked page are out of date; `marketplace/packages/crm/tests/test_grants_import.py`
runs it and imports the CSV through the CRM worker.
