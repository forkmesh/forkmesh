# name: Review my changes
# description: Review the work on this branch like a senior engineer: bugs first, then risk, speed and clarity, each with a concrete fix.

Review the changes on the current branch as a senior engineer would.

1. Find what changed: compare the branch with the default branch it started
   from, and include uncommitted work. Read the surrounding code, the tests
   and the project's own rules (README, contributing notes, agent
   instructions) before judging anything.
2. Say in two or three sentences what the change is meant to do and any
   assumption you are making about it.
3. Look for, in this order:
   - bugs: wrong results, unhandled edge cases, races, resource leaks, broken
     error handling, data loss;
   - security: untrusted input, secrets, permissions, injection;
   - performance: repeated or needless work, poor data structures, avoidable
     I/O, anything that will not scale with real input;
   - maintainability: logic that could be simpler, missing or weak tests,
     names or structure that hide what the code does.
4. For each finding give the file and line, why it is a problem, its impact
   (Critical, High, Medium or Low) and a concrete fix, with code where it
   helps. Mention the trade-off when the fix has one. Skip pure style
   preferences, and say which parts are well done and should stay as they
   are.

Do not change any files unless I ask you to.

Finish with the three most important fixes and the tests or measurements
that would prove each one.
