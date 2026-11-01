# Spreadsheets

A spreadsheet editor that any account installs for itself: workbooks with
several sheets, formulas, formatting, charts, filters, pivot tables and an AI
assistant. It opens Excel `.xls` and `.xlsx`, OpenDocument `.ods`, Excel XML,
web page tables, CSV and TSV, and downloads `.xlsx`, `.xls`, `.ods`, CSV, TSV,
HTML, JSON, Markdown and PDF (through print). Workbooks are saved to the
account, sealed under a key minted for that account.

How it works, what each format keeps, and the limits:
[docs/marketplace/spreadsheets.md](../../../docs/marketplace/spreadsheets.md).

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Web | `content/web/view.html`, `content/web/page.js` | `/dashboard/spreadsheets`; the whole editor and every file format run in the browser. |
| Worker | `content/worker/spreadsheets.py`, `/api/spreadsheets` | Account-scoped, sealed per account, revision-checked saves, AI operations. |
| Desktop | `content/desktop/SpreadsheetsLink.cpp` | Rail button **Spreadsheets** that opens the web page on the active host. |

## Test

```sh
python3 -m pytest -q marketplace/packages/spreadsheets/tests
node marketplace/packages/spreadsheets/tests/engine_checks.js
```
