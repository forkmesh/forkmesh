# name: Refresh the documentation
# description: Find one coherent set of documentation drift, correct it from the code and tests, and leave a review-ready update.

Do one focused documentation pass. This is implementation work: when the
evidence supports a correction, update the repository rather than stopping at
a report.

1. Find the documentation's entry points (README, a docs folder and its
   index, contributing notes, generated or public docs) and read the ones for
   the area you pick before editing.
2. Establish what is true now from recent commits, releases, tests and the
   code itself. Treat old posts, plans, screenshots and generated copy as
   history, not current truth.
3. Choose one coherent set of drift that matters to users: a setup step, a
   command, a route or API, a supported feature or limitation, a security or
   privacy statement, or behaviour that differs between clients. Correct it
   everywhere the claim is repeated rather than making scattered edits in
   unrelated areas.
4. State behaviour precisely. Distinguish released from unreleased, hosted
   from self-hosted, and automatic from user-configured where it matters.
   Keep limitations and compatibility notes. Never claim more than the code
   and tests prove.
5. Update the main technical page first, then the README, examples, FAQs and
   linked copy. Keep historical records; add a dated note about current
   behaviour instead of silently rewriting what an old release did.
6. Add a check when stale wording could come back and mislead people about
   security, privacy, availability or compatibility. Rebuild generated docs
   from their sources and run the docs, link and rendering checks the project
   has.
7. Check the final diff for code changes and unrelated files. Leave a small
   review-ready commit on the branch; do not open a pull request, merge or
   deploy it.

Finish with what you checked, the drift found, every place corrected, the
behaviour and limitations now documented, the checks you ran, and the next
riskiest documentation gap.
