# Strata

Architecture and complexity for any repository you open in ForkMesh: a layered
module dependency graph, a dependency structure matrix, the abstractness ×
instability diagram, a metrics treemap, code rules with technical debt, an A
to E rating and quality gates. Our own take on NDepend, for every language
ForkMesh reads.

## Using it

1. Add **Strata** from the Market; it appears on the rail.
2. It analyzes the repository you have open. Pick another one in the header,
   or **Folder…** to analyze any folder. **Re-analyze** reads it again.
3. **Modules** sets how many folder levels make a module (Auto picks one).
4. Switch views with the tabs. Click a module or file to see its numbers in
   the details panel; double-click a module in the graph to see its files;
   double-click a file or a violation to open it.
5. **Export report…** saves everything as one HTML file.

## How the scores work

- Each rule violation carries an estimated time to fix it (debt), larger the
  further the code is over the rule's threshold.
- Debt ratio = debt ÷ the effort to write the code (18 days per 1,000 lines).
  A ≤ 5 %, B ≤ 10 %, C ≤ 20 %, D ≤ 50 %, E above.
- Annual interest estimates the time lost each year because files with debt
  keep changing (5 % of a file's debt per change).
- Quality gates: the rating (warn at D, fail at E), no critical violation, no
  module cycle, no function above complexity 50, and the debt ratio of the 10 %
  most-changed files below 20 %.

The full rule table and the layout algorithm are in
[docs/marketplace/strata.md](../../../docs/marketplace/strata.md).
