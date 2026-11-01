# Surveys

Surveys is an optional ForkMesh organization plugin. Members build surveys
from choice, text and rating questions, open them, share a private link that
anyone can answer without an account, and read the results as they arrive.

- Questions: pick one, pick any, short text, long text, and a 1-to-N rating.
- Surveys and responses are encrypted and shared only with members of the
  organization.
- Everything can be managed on the web (laid out for phones too), in a native
  desktop page, and in the iPhone and Android apps.
- Results show option counts and percentages, rating averages and spread, and
  the newest text answers; every response can be read, deleted, or exported as
  CSV.
- A survey can open and close at set times, stop after a number of responses,
  accept one response per person, shuffle its choices, and ping its creator
  when responses arrive.

Install and uninstall use the shared organization plugin lifecycle. Uninstall
offers a JSON export, deletes the organization's surveys and responses, stops
every shared link from working, and reverses the package schema when the
final installation is removed. Developer notes are in
[docs/marketplace/surveys.md](../../../docs/marketplace/surveys.md).
