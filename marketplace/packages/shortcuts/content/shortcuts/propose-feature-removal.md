# name: Propose a feature to retire
# description: Nominate the one feature whose evidence says it is unused or no longer worth what it costs, and file the case as an issue.

Nominate one feature for retirement and make the case in an issue. You
propose; you never remove. A run that nominates nothing because the evidence
does not support a removal is a good run, and saying so is the value of the
report.

A feature is something the product offers: a page or panel, a menu action, a
setting, a flag, a command, an endpoint, an extension, a documented
capability. Dead functions and duplicated helpers are a code cleanup, not a
feature removal.

1. Build the candidate list from what the project promises (README, docs,
   website) and what it actually ships (screens, settings, feature flags,
   routes, commands, packages). Cross-read the demand side: issues and
   discussions for who asked for it and who complained, and the git history
   for when it was last meaningfully changed, whether it was ever finished,
   and whether anything was built on it since.
2. Look for use, not just callers: usage analytics, logs, support requests
   and the tests that exercise the feature. Record dates and counts, not
   impressions, and say whether the data covers everyone or one machine.
3. Weigh both sides. Keeping it costs code and dependencies, interface space,
   docs and tests, support, runtime cost and security exposure. It delivers
   value to whoever depends on it, a documented promise, a compatibility
   path, or plumbing another feature uses.
4. Attack your own evidence before presenting it, because a wrong nomination
   costs a working feature:
   - "Nothing calls it" is not proof. Check dynamic invocation, names wired
     up at runtime, generated code, resources, scripts, packaging and CI,
     configuration keys, feature flags, stored settings, migrations, wire
     formats, extensions and public APIs.
   - A young or unannounced feature with no adoption is unfinished, not
     dead: the honest proposal is to finish it, announce it or park it.
   - Never nominate security, privacy, reliability, accessibility, logging,
     audit history, migration or compatibility paths, or their tests, just
     because the usual path never reaches them.
5. Pick exactly one: the strongest case, not the biggest deletion. If nothing
   clears the bar, nominate nothing and report the two closest candidates
   with the evidence that would settle them.
6. Search open and closed issues first, then file the case or comment on the
   existing issue. State what the feature is and where it lives (`path:line`),
   the evidence it is unused, what keeping it costs, who and what would
   break, the deprecation and data path a removal needs, what removal saves,
   alternatives short of removal (shrink it, hide it behind a flag, merge it
   into a neighbour, or document it), your confidence, and what evidence
   would change your mind. Use labels that already exist.

Stay read-only over the code: do not delete anything, open a branch or a pull
request, or close or relabel anyone else's issue.

Finish with the nomination in one line, its evidence, the cost of keeping it
against the cost of removing it, the risks and migration path, the issue you
filed or commented on, the runners-up, and what to re-check next time.
