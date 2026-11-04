export const meta = {
  name: 'fix-github-actions',
  description: 'Find the GitHub Actions workflows failing on main, diagnose each root cause, fix it in this worktree and verify',
  whenToUse: 'When tools/github_actions_status.py reports failing workflows on the forkmesh/forkmesh mirror and the cause should be fixed on the current branch.',
  phases: [
    { title: 'Diagnose', detail: 'read run state, reproduce each failure locally, group by root cause' },
    { title: 'Fix', detail: 'one agent per root cause, in turn, in the current worktree' },
    { title: 'Verify', detail: 'rerun the failing command and check the fix keeps the test meaningful' },
    { title: 'Recheck', detail: 'rerun every reproduction command together' },
  ],
}

const CAUSES_SCHEMA = {
  type: 'object',
  properties: {
    failing: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          workflow: { type: 'string' },
          job: { type: 'string' },
          step: { type: 'string' },
          annotation: { type: 'string' },
          runUrl: { type: 'string' },
        },
        required: ['workflow', 'job', 'step', 'annotation'],
      },
    },
    causes: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          id: { type: 'string' },
          summary: { type: 'string' },
          workflows: { type: 'array', items: { type: 'string' } },
          reproduce: { type: 'string' },
          reproducedLocally: { type: 'boolean' },
          localOutput: { type: 'string' },
          introducedBy: { type: 'string' },
          verdict: { type: 'string', enum: ['code-bug', 'outdated-test', 'outdated-manifest-or-data', 'flake', 'ci-environment', 'unknown'] },
          plan: { type: 'string' },
          files: { type: 'array', items: { type: 'string' } },
        },
        required: ['id', 'summary', 'workflows', 'reproduce', 'reproducedLocally', 'verdict', 'plan', 'files'],
      },
    },
    alreadyFixedOnBranch: { type: 'array', items: { type: 'string' } },
  },
  required: ['failing', 'causes'],
}

const FIX_SCHEMA = {
  type: 'object',
  properties: {
    id: { type: 'string' },
    fixed: { type: 'boolean' },
    changedFiles: { type: 'array', items: { type: 'string' } },
    whatChanged: { type: 'string' },
    whyThisSide: { type: 'string' },
    reproduceOutputAfter: { type: 'string' },
    blocker: { type: 'string' },
  },
  required: ['id', 'fixed', 'changedFiles', 'whatChanged', 'whyThisSide'],
}

const VERDICT_SCHEMA = {
  type: 'object',
  properties: {
    passes: { type: 'boolean' },
    testStillMeaningful: { type: 'boolean' },
    problems: { type: 'array', items: { type: 'string' } },
    output: { type: 'string' },
  },
  required: ['passes', 'testStillMeaningful', 'problems'],
}

const RECHECK_SCHEMA = {
  type: 'object',
  properties: {
    allPass: { type: 'boolean' },
    results: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          id: { type: 'string' },
          command: { type: 'string' },
          passed: { type: 'boolean' },
          tail: { type: 'string' },
        },
        required: ['id', 'command', 'passed'],
      },
    },
  },
  required: ['allPass', 'results'],
}

const ENV_RULES = `Run every test subprocess with FORKMESH_HOME and FORKMESH_PROJECTS_DIR unset (env -u ...) and GIT_CEILING_DIRECTORIES="$FORKMESH_AGENT_SCRATCH"; put logs and throwaway files in $FORKMESH_AGENT_SCRATCH, never in the worktree. Run pytest with -q --tb=short -p no:cacheprovider. Anything over a minute goes in a background Bash call that you wait on.`

const focus = typeof args === 'string' && args.trim() ? `\nThe caller asked to focus on: ${args.trim()}\n` : ''

phase('Diagnose')
const diagnosis = await agent(
  `Find out which GitHub Actions workflows fail on main of the forkmesh/forkmesh GitHub mirror and why.${focus}

1. Run \`python3 tools/github_actions_status.py --json\` (and the plain form for the summary). gh is not installed and job logs need a login; the failed job, step and first ::error annotation are readable without one (docs/engineering/github-actions.md). For more annotations use the public REST API: /repos/forkmesh/forkmesh/actions/runs/<run id>/jobs and /repos/forkmesh/forkmesh/check-runs/<job id>/annotations.
2. For each failing workflow, read its file in .github/workflows/ and find the exact command the failing step runs. Reproduce it locally in this worktree. A quality-gate failure names the gate id; tools/quality_gate.py and its gate list say which command that gate runs.
3. Group the failures by root cause: one broken test usually fails several workflows and every OS in a matrix. If only one OS fails and the step passes elsewhere, or the same commit passed on a rerun, call it a flake.
4. For each cause, use git log -S / git log on the touched files to find the commit that introduced it, then decide which side is wrong: the code (a regression: verdict code-bug), the test asserting behaviour that was changed on purpose (outdated-test), or a checked-in manifest, fixture or generated file that was not refreshed (outdated-manifest-or-data). Say why in plan, and list the files the fix will touch.
5. If a failure no longer reproduces on this branch because it is already fixed here, list it in alreadyFixedOnBranch instead of causes.

${ENV_RULES}
Do not edit any file.`,
  { label: 'diagnose', phase: 'Diagnose', schema: CAUSES_SCHEMA },
)

if (!diagnosis) {
  return { error: 'diagnosis agent returned nothing' }
}

log(`${diagnosis.failing.length} failing job(s), ${diagnosis.causes.length} root cause(s)`)
const actionable = diagnosis.causes.filter(c => c.verdict !== 'flake' && c.verdict !== 'ci-environment')
const skipped = diagnosis.causes.filter(c => !actionable.includes(c))
if (skipped.length) {
  log(`Not fixing ${skipped.length} cause(s) judged flake or CI environment: ${skipped.map(c => c.id).join(', ')}`)
}

const outcomes = []
for (const cause of actionable) {
  const fix = await agent(
    `Fix one root cause of failing GitHub Actions on main, in this worktree.

Cause ${cause.id}: ${cause.summary}
Workflows: ${cause.workflows.join(', ')}
Reproduce with: ${cause.reproduce}
Local output: ${(cause.localOutput || '').slice(0, 4000)}
Introduced by: ${cause.introducedBy || 'unknown'}
Diagnosis verdict: ${cause.verdict}
Plan: ${cause.plan}
Files: ${cause.files.join(', ')}

Check the diagnosis before acting on it. Fix the side that is wrong: if the code regressed, fix the code and leave the test alone; if the test asserts behaviour that was changed on purpose, update the test to the new behaviour without loosening what it protects; if a manifest, fixture or generated file is stale, refresh it with the tool that owns it (and re-render any generated docs). Never skip, xfail, delete or weaken a check just to make it pass, and never edit a workflow file to stop running a check.
Earlier fixes in this run may already have changed files; build on them and do not revert them.
Follow AGENTS.md: no new comments or docstrings in source, explain the reason in the matching docs/*.md page when the change needs explaining, and add a changelog line only if a user would notice the change.
Rerun the reproduce command and include the tail of its output. Do not commit.

${ENV_RULES}`,
    { label: `fix:${cause.id}`, phase: 'Fix', schema: FIX_SCHEMA },
  )
  if (!fix) {
    outcomes.push({ cause, fix: null, verdict: null })
    continue
  }

  const verdict = await agent(
    `Independently check a fix for a failing GitHub Actions check in this worktree. Assume it is wrong until the evidence says otherwise.

Cause ${cause.id}: ${cause.summary}
Reproduce with: ${cause.reproduce}
The fixer says it changed ${fix.changedFiles.join(', ') || 'nothing'}: ${fix.whatChanged}
Why that side: ${fix.whyThisSide}

1. Read the diff of those files (git diff -- <path>, one at a time).
2. Rerun the reproduce command yourself; passes is true only if it exits cleanly.
3. testStillMeaningful is false if the fix skipped, xfailed, deleted or loosened an assertion, widened an allowlist with no reason, or edited a workflow so the check no longer runs, or if the code was the side that was wrong and the test was bent to match it.
4. List every problem you find. Do not edit files.

${ENV_RULES}`,
    { label: `verify:${cause.id}`, phase: 'Verify', schema: VERDICT_SCHEMA },
  )
  outcomes.push({ cause, fix, verdict })
  log(`${cause.id}: ${verdict && verdict.passes && verdict.testStillMeaningful ? 'verified' : 'needs attention'}`)
}

phase('Recheck')
const commands = actionable.map(c => `- ${c.id}: ${c.reproduce}`).join('\n')
const recheck = actionable.length
  ? await agent(
    `Rerun every reproduction command below in this worktree, after all fixes, and report each result with the tail of its output. Also run git status --short and make sure no stray file (log, scratch script, .pytest_cache change) was left in the worktree; report any as a failed result with id "stray-files". Do not edit files.

${commands}

${ENV_RULES}`,
    { label: 'recheck', phase: 'Recheck', schema: RECHECK_SCHEMA },
  )
  : { allPass: true, results: [] }

return {
  failing: diagnosis.failing,
  alreadyFixedOnBranch: diagnosis.alreadyFixedOnBranch || [],
  notFixed: skipped.map(c => ({ id: c.id, verdict: c.verdict, summary: c.summary })),
  outcomes: outcomes.map(o => ({
    id: o.cause.id,
    summary: o.cause.summary,
    workflows: o.cause.workflows,
    fixed: Boolean(o.fix && o.fix.fixed),
    changedFiles: o.fix ? o.fix.changedFiles : [],
    whatChanged: o.fix ? o.fix.whatChanged : '',
    verified: Boolean(o.verdict && o.verdict.passes && o.verdict.testStillMeaningful),
    problems: o.verdict ? o.verdict.problems : ['fix or verify agent returned nothing'],
  })),
  recheck,
}
