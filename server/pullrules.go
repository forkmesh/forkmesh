package mirrornode

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

const (
	pullRulesPolicyPath      = ".forkmesh/pull-rules.json"
	pullRulesSchema          = "forkmesh-pull-rules-v1"
	pullRulesEvaluateType    = "forkmesh.pull-rules-evaluate-v1"
	maxPullRulesPolicy       = 32 << 10
	maxPullRulesRequest      = 48 << 10
	maxPullRules             = 32
	maxPullRulePatterns      = 32
	maxPullRuleModels        = 16
	maxPullRuleBranches      = 16
	maxPullRuleCommits       = 1000
	maxPullRuleProblems      = 3
	maxPullRuleDiffBytes     = 8 << 20
	maxPullRuleNumstatBytes  = 2 << 20
	pullRuleSourceRepository = "repository"
	pullRuleSourceOrg        = "organization"
)

var (
	pullRuleIDPattern     = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{0,47}$`)
	pullRuleBranchPattern = regexp.MustCompile(`^[A-Za-z0-9*?][A-Za-z0-9._/*?-]{0,199}$`)
	pullRuleHunkPattern   = regexp.MustCompile(`^@@ -[0-9,]+ \+([0-9]+)`)
	pullRuleKinds         = map[string]bool{
		"tests": true, "docs": true, "size": true, "paths": true,
		"secrets": true, "commits": true, "model": true,
	}
	defaultPullRuleSourcePaths = []string{
		"*.go", "*.py", "*.js", "*.mjs", "*.ts", "*.tsx", "*.jsx", "*.c", "*.cc", "*.cpp", "*.h",
		"*.hpp", "*.java", "*.kt", "*.swift", "*.rs", "*.rb", "*.cs", "*.php", "*.m", "*.mm",
	}
	defaultPullRuleTestPaths = []string{
		"**/test/**", "**/tests/**", "**/__tests__/**", "**/testdata/**", "*_test.*", "test_*",
		"*.test.*", "*.spec.*", "*Test.*", "*Tests.*",
	}
	defaultPullRuleDocsPaths = []string{"**/docs/**", "*.md", "*.rst", "*.adoc"}
	builtinSecretPatterns    = []struct {
		name    string
		pattern *regexp.Regexp
	}{
		{"private key", regexp.MustCompile(`-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY`)},
		{"AWS access key", regexp.MustCompile(`\b(?:AKIA|ASIA)[0-9A-Z]{16}\b`)},
		{"GitHub token", regexp.MustCompile(`\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{22,})`)},
		{"Slack token", regexp.MustCompile(`\bxox[abposr]-[A-Za-z0-9-]{10,}`)},
		{"Anthropic API key", regexp.MustCompile(`\bsk-ant-[A-Za-z0-9_-]{20,}`)},
		{"OpenAI API key", regexp.MustCompile(`\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}`)},
		{"Google API key", regexp.MustCompile(`\bAIza[0-9A-Za-z_-]{35}\b`)},
		{"Stripe secret key", regexp.MustCompile(`\b[rs]k_live_[0-9A-Za-z]{24,}`)},
	}
)

type PullRulesPolicy struct {
	Schema string     `json:"schema"`
	Rules  []PullRule `json:"rules"`
}

type PullRule struct {
	ID               string   `json:"id"`
	Title            string   `json:"title,omitempty"`
	Kind             string   `json:"kind"`
	Branches         []string `json:"branches,omitempty"`
	Enforcement      string   `json:"enforcement,omitempty"`
	Suggestion       string   `json:"suggestion,omitempty"`
	Paths            []string `json:"paths,omitempty"`
	TestPaths        []string `json:"testPaths,omitempty"`
	DocsPaths        []string `json:"docsPaths,omitempty"`
	MaxFiles         int      `json:"maxFiles,omitempty"`
	MaxLines         int      `json:"maxLines,omitempty"`
	Patterns         []string `json:"patterns,omitempty"`
	SubjectPattern   string   `json:"subjectPattern,omitempty"`
	MaxSubjectLength int      `json:"maxSubjectLength,omitempty"`
	Models           []string `json:"models,omitempty"`

	branches  []*regexp.Regexp
	paths     []*regexp.Regexp
	testPaths []*regexp.Regexp
	docsPaths []*regexp.Regexp
	patterns  []*regexp.Regexp
	subject   *regexp.Regexp
}

type PullRuleResult struct {
	ID          string   `json:"id"`
	Title       string   `json:"title"`
	Kind        string   `json:"kind"`
	Source      string   `json:"source"`
	Enforcement string   `json:"enforcement"`
	Status      string   `json:"status"`
	Problems    []string `json:"problems"`
	Suggestion  string   `json:"suggestion"`
}

type PullRulesReport struct {
	Available bool             `json:"available"`
	Branch    string           `json:"branch"`
	Passed    bool             `json:"passed"`
	Blocked   bool             `json:"blocked"`
	Rules     []PullRuleResult `json:"rules"`
}

func validRuleText(value string, maximum int) bool {
	if len(value) > maximum || !utf8.ValidString(value) {
		return false
	}
	for _, r := range value {
		if unicode.IsControl(r) && r != '\n' {
			return false
		}
	}
	return true
}

func compilePathGlob(pattern string, basename bool) (*regexp.Regexp, error) {
	if pattern == "" || len(pattern) > 200 || !utf8.ValidString(pattern) || strings.ContainsAny(pattern, "\x00\n") {
		return nil, errors.New("invalid path pattern")
	}
	var builder strings.Builder
	builder.WriteString("^")
	if basename && !strings.Contains(pattern, "/") {
		builder.WriteString("(?:.*/)?")
	}
	for i := 0; i < len(pattern); i++ {
		switch {
		case strings.HasPrefix(pattern[i:], "**/"):
			builder.WriteString("(?:.*/)?")
			i += 2
		case strings.HasPrefix(pattern[i:], "**"):
			builder.WriteString(".*")
			i++
		case pattern[i] == '*':
			builder.WriteString("[^/]*")
		case pattern[i] == '?':
			builder.WriteString("[^/]")
		default:
			builder.WriteString(regexp.QuoteMeta(pattern[i : i+1]))
		}
	}
	builder.WriteString("$")
	return regexp.Compile(builder.String())
}

func compilePathGlobs(patterns []string, defaults []string) ([]*regexp.Regexp, error) {
	if len(patterns) > maxPullRulePatterns {
		return nil, errors.New("too many path patterns")
	}
	if len(patterns) == 0 {
		patterns = defaults
	}
	compiled := make([]*regexp.Regexp, 0, len(patterns))
	for _, pattern := range patterns {
		re, err := compilePathGlob(pattern, true)
		if err != nil {
			return nil, err
		}
		compiled = append(compiled, re)
	}
	return compiled, nil
}

func matchesAny(patterns []*regexp.Regexp, value string) bool {
	for _, pattern := range patterns {
		if pattern.MatchString(value) {
			return true
		}
	}
	return false
}

func (r *PullRule) compile() error {
	if !pullRuleIDPattern.MatchString(r.ID) {
		return fmt.Errorf("rule id %q is invalid", r.ID)
	}
	if !pullRuleKinds[r.Kind] {
		return fmt.Errorf("rule %s has an unknown kind", r.ID)
	}
	if !validRuleText(r.Title, 120) || strings.Contains(r.Title, "\n") || !validRuleText(r.Suggestion, 600) {
		return fmt.Errorf("rule %s has an invalid title or suggestion", r.ID)
	}
	if r.Enforcement != "" && r.Enforcement != "block" && r.Enforcement != "warn" {
		return fmt.Errorf("rule %s enforcement must be block or warn", r.ID)
	}
	if len(r.Branches) > maxPullRuleBranches {
		return fmt.Errorf("rule %s names too many branches", r.ID)
	}
	for _, branch := range r.Branches {
		if !pullRuleBranchPattern.MatchString(branch) {
			return fmt.Errorf("rule %s has an invalid branch pattern", r.ID)
		}
		re, err := compilePathGlob(branch, false)
		if err != nil {
			return fmt.Errorf("rule %s has an invalid branch pattern", r.ID)
		}
		r.branches = append(r.branches, re)
	}
	var err error
	switch r.Kind {
	case "tests":
		if r.paths, err = compilePathGlobs(r.Paths, defaultPullRuleSourcePaths); err == nil {
			r.testPaths, err = compilePathGlobs(r.TestPaths, defaultPullRuleTestPaths)
		}
	case "docs":
		if r.paths, err = compilePathGlobs(r.Paths, defaultPullRuleSourcePaths); err == nil {
			r.docsPaths, err = compilePathGlobs(r.DocsPaths, defaultPullRuleDocsPaths)
		}
	case "paths":
		if len(r.Paths) == 0 {
			return fmt.Errorf("rule %s names no protected paths", r.ID)
		}
		r.paths, err = compilePathGlobs(r.Paths, nil)
	case "size":
		if r.MaxFiles < 0 || r.MaxLines < 0 || r.MaxFiles > 100000 || r.MaxLines > 10000000 ||
			(r.MaxFiles == 0 && r.MaxLines == 0) {
			return fmt.Errorf("rule %s needs maxFiles or maxLines", r.ID)
		}
	case "secrets":
		if len(r.Patterns) > 16 {
			return fmt.Errorf("rule %s has too many secret patterns", r.ID)
		}
		for _, pattern := range r.Patterns {
			re, compileErr := regexp.Compile(pattern)
			if compileErr != nil || pattern == "" || len(pattern) > 200 {
				return fmt.Errorf("rule %s has an invalid secret pattern", r.ID)
			}
			r.patterns = append(r.patterns, re)
		}
		if len(r.Paths) > 0 {
			r.paths, err = compilePathGlobs(r.Paths, nil)
		}
	case "commits":
		if r.MaxSubjectLength < 0 || r.MaxSubjectLength > 1000 || len(r.SubjectPattern) > 200 ||
			(r.SubjectPattern == "" && r.MaxSubjectLength == 0) {
			return fmt.Errorf("rule %s needs subjectPattern or maxSubjectLength", r.ID)
		}
		if r.SubjectPattern != "" {
			if r.subject, err = regexp.Compile(r.SubjectPattern); err != nil {
				return fmt.Errorf("rule %s has an invalid subject pattern", r.ID)
			}
		}
	case "model":
		if len(r.Models) == 0 || len(r.Models) > maxPullRuleModels {
			return fmt.Errorf("rule %s names 1-16 models", r.ID)
		}
		seen := map[string]bool{}
		for _, model := range r.Models {
			if !agentModelPattern.MatchString(model) || seen[model] {
				return fmt.Errorf("rule %s has an invalid model", r.ID)
			}
			seen[model] = true
		}
	}
	if err != nil {
		return fmt.Errorf("rule %s: %v", r.ID, err)
	}
	return nil
}

func ParsePullRulesPolicy(raw []byte) (*PullRulesPolicy, error) {
	var policy PullRulesPolicy
	if err := decodeStrictJSON(raw, maxPullRulesPolicy, &policy); err != nil {
		return nil, err
	}
	if policy.Schema != pullRulesSchema {
		return nil, errors.New("unsupported rules schema")
	}
	if len(policy.Rules) > maxPullRules {
		return nil, errors.New("a policy names at most 32 rules")
	}
	seen := map[string]bool{}
	for i := range policy.Rules {
		if err := policy.Rules[i].compile(); err != nil {
			return nil, err
		}
		if seen[policy.Rules[i].ID] {
			return nil, fmt.Errorf("rule id %s is used twice", policy.Rules[i].ID)
		}
		seen[policy.Rules[i].ID] = true
	}
	return &policy, nil
}

func (r *PullRule) appliesTo(branch string) bool {
	return len(r.branches) == 0 || matchesAny(r.branches, branch)
}

func (r *PullRule) enforcement() string {
	if r.Enforcement == "warn" {
		return "warn"
	}
	return "block"
}

func defaultPullRuleTitle(kind string) string {
	return map[string]string{
		"tests": "Tests accompany code changes", "docs": "Documentation accompanies code changes",
		"size": "Pull request size", "paths": "Protected paths", "secrets": "No secrets in the change",
		"commits": "Commit message format", "model": "Required AI model",
	}[kind]
}

func (r *PullRule) suggestion() string {
	if r.Suggestion != "" {
		return r.Suggestion
	}
	switch r.Kind {
	case "tests":
		return "Add or update tests that cover the changed code."
	case "docs":
		return "Update the documentation to describe this change."
	case "size":
		parts := []string{}
		if r.MaxFiles > 0 {
			parts = append(parts, fmt.Sprintf("%d files", r.MaxFiles))
		}
		if r.MaxLines > 0 {
			parts = append(parts, fmt.Sprintf("%d changed lines", r.MaxLines))
		}
		return "Split this pull request into smaller ones of at most " + strings.Join(parts, " and ") + "."
	case "paths":
		return "Drop the changes to protected paths, or ask an owner to make them in a separate pull request."
	case "secrets":
		return "Remove the secret from every commit on the branch (rewrite the history, not just the tip) and rotate it: it is already exposed."
	case "commits":
		return "Reword the commit subjects (git rebase -i, then reword) so each one follows the rule."
	case "model":
		return "Make the change with " + strings.Join(r.Models, " or ") +
			" on a ForkMesh runner: only commits a ForkMesh runner signed under a router-issued model grant prove which model wrote them."
	}
	return ""
}

type pullRuleChange struct {
	path    string
	added   int
	deleted int
}

type pullRuleInput struct {
	gitDir      string
	base        string
	head        string
	mergeBase   string
	branch      string
	routerKey   string
	changes     []pullRuleChange
	changesErr  error
	commits     []rawCommitRecord
	commitsErr  error
	diffLoaded  bool
	added       []pullRuleAddedLine
	diffErr     error
	loadedFiles bool
	loadedLog   bool
}

type rawCommitRecord struct {
	oid    string
	commit rawCommit
}

type pullRuleAddedLine struct {
	path string
	line int
	text string
}

func pullRuleGit(ctx context.Context, gitDir string, stdin []byte, limit int, args ...string) ([]byte, error) {
	cmd := exec.CommandContext(ctx, "git", append([]string{"--git-dir=" + gitDir}, args...)...)
	cmd.Env = pullGitEnvironment(nil)
	if stdin != nil {
		cmd.Stdin = bytes.NewReader(stdin)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stderr = io.Discard
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	output, readErr := io.ReadAll(io.LimitReader(stdout, int64(limit)+1))
	if len(output) > limit {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, errors.New("the change is too large to check")
	}
	if err := cmd.Wait(); err != nil || readErr != nil {
		return nil, errors.New("git could not read the change")
	}
	return output, nil
}

func (in *pullRuleInput) files(ctx context.Context) ([]pullRuleChange, error) {
	if in.loadedFiles {
		return in.changes, in.changesErr
	}
	in.loadedFiles = true
	raw, err := pullRuleGit(ctx, in.gitDir, nil, maxPullRuleNumstatBytes,
		"diff", "--numstat", "-z", "--no-renames", "--no-ext-diff", in.mergeBase, in.head)
	if err != nil {
		in.changesErr = err
		return nil, err
	}
	for _, record := range strings.Split(string(raw), "\x00") {
		fields := strings.SplitN(record, "\t", 3)
		if len(fields) != 3 || fields[2] == "" {
			continue
		}
		added, _ := strconv.Atoi(fields[0])
		deleted, _ := strconv.Atoi(fields[1])
		in.changes = append(in.changes, pullRuleChange{path: fields[2], added: added, deleted: deleted})
	}
	return in.changes, nil
}

func (in *pullRuleInput) log(ctx context.Context) ([]rawCommitRecord, error) {
	if in.loadedLog {
		return in.commits, in.commitsErr
	}
	in.loadedLog = true
	listing, err := pullRuleGit(ctx, in.gitDir, nil, 256<<10, "rev-list", "--reverse",
		fmt.Sprintf("--max-count=%d", maxPullRuleCommits+1), in.head, "^"+in.base)
	if err != nil {
		in.commitsErr = err
		return nil, err
	}
	oids := strings.Fields(string(listing))
	if len(oids) > maxPullRuleCommits {
		in.commitsErr = fmt.Errorf("the pull request has more than %d commits to check", maxPullRuleCommits)
		return nil, in.commitsErr
	}
	if len(oids) == 0 {
		return nil, nil
	}
	batch, err := pullRuleGit(ctx, in.gitDir, []byte(strings.Join(oids, "\n")+"\n"), 64<<20, "cat-file", "--batch")
	if err != nil {
		in.commitsErr = err
		return nil, err
	}
	reader := bufio.NewReader(bytes.NewReader(batch))
	for _, oid := range oids {
		header, err := reader.ReadString('\n')
		fields := strings.Fields(header)
		if err != nil || len(fields) != 3 || fields[0] != oid || fields[1] != "commit" {
			in.commitsErr = errors.New("git could not read a commit")
			return nil, in.commitsErr
		}
		size, err := strconv.Atoi(fields[2])
		if err != nil || size < 0 || size > maxAttestedCommitBytes {
			in.commitsErr = errors.New("a commit is too large to check")
			return nil, in.commitsErr
		}
		body := make([]byte, size+1)
		if _, err := io.ReadFull(reader, body); err != nil {
			in.commitsErr = errors.New("git could not read a commit")
			return nil, in.commitsErr
		}
		commit, ok := parseRawCommit(body[:size])
		if !ok {
			in.commitsErr = errors.New("a commit could not be parsed")
			return nil, in.commitsErr
		}
		in.commits = append(in.commits, rawCommitRecord{oid: oid, commit: commit})
	}
	return in.commits, nil
}

func (in *pullRuleInput) addedLines(ctx context.Context) ([]pullRuleAddedLine, error) {
	if in.diffLoaded {
		return in.added, in.diffErr
	}
	in.diffLoaded = true
	raw, err := pullRuleGit(ctx, in.gitDir, nil, maxPullRuleDiffBytes, "diff", "--no-color",
		"--no-ext-diff", "--no-renames", "--unified=0", in.mergeBase, in.head)
	if err != nil {
		in.diffErr = err
		return nil, err
	}
	path, line := "", 0
	for _, text := range strings.Split(string(raw), "\n") {
		switch {
		case strings.HasPrefix(text, "+++ "):
			path = strings.TrimPrefix(strings.TrimPrefix(text, "+++ "), "b/")
		case strings.HasPrefix(text, "@@ "):
			if match := pullRuleHunkPattern.FindStringSubmatch(text); match != nil {
				line, _ = strconv.Atoi(match[1])
			}
		case strings.HasPrefix(text, "+"):
			in.added = append(in.added, pullRuleAddedLine{path: path, line: line, text: text[1:]})
			line++
		}
	}
	return in.added, nil
}

func shortOid(oid string) string {
	if len(oid) > 10 {
		return oid[:10]
	}
	return oid
}

func boundedProblems(problems []string, more int) []string {
	if len(problems) > maxPullRuleProblems {
		more += len(problems) - maxPullRuleProblems
		problems = problems[:maxPullRuleProblems]
	}
	for i := range problems {
		problems[i] = tailRunes(problems[i], 240)
	}
	if more > 0 {
		problems = append(problems, fmt.Sprintf("…and %d more.", more))
	}
	return problems
}

func (in *pullRuleInput) cleanMerge(ctx context.Context, commit rawCommit) bool {
	if len(commit.parents) != 2 {
		return false
	}
	output, err := pullRuleGit(ctx, in.gitDir, nil, 4096, "merge-tree", "--write-tree", "--no-messages",
		commit.parents[0], commit.parents[1])
	if err != nil {
		return false
	}
	tree := strings.TrimSpace(strings.SplitN(string(output), "\n", 2)[0])
	return tree == commit.tree
}

func (r *PullRule) evaluate(ctx context.Context, in *pullRuleInput) []string {
	switch r.Kind {
	case "tests", "docs":
		changes, err := in.files(ctx)
		if err != nil {
			return []string{err.Error()}
		}
		wanted := r.testPaths
		if r.Kind == "docs" {
			wanted = r.docsPaths
		}
		var triggered []string
		for _, change := range changes {
			if matchesAny(wanted, change.path) {
				return nil
			}
			if matchesAny(r.paths, change.path) {
				triggered = append(triggered, change.path)
			}
		}
		if len(triggered) == 0 {
			return nil
		}
		what := "test"
		if r.Kind == "docs" {
			what = "documentation"
		}
		problem := fmt.Sprintf("%d changed code file(s) but no %s file changed (for example %s).",
			len(triggered), what, triggered[0])
		return []string{problem}
	case "size":
		changes, err := in.files(ctx)
		if err != nil {
			return []string{err.Error()}
		}
		lines := 0
		for _, change := range changes {
			lines += change.added + change.deleted
		}
		var problems []string
		if r.MaxFiles > 0 && len(changes) > r.MaxFiles {
			problems = append(problems, fmt.Sprintf("%d files changed; the limit is %d.", len(changes), r.MaxFiles))
		}
		if r.MaxLines > 0 && lines > r.MaxLines {
			problems = append(problems, fmt.Sprintf("%d lines changed; the limit is %d.", lines, r.MaxLines))
		}
		return problems
	case "paths":
		changes, err := in.files(ctx)
		if err != nil {
			return []string{err.Error()}
		}
		var problems []string
		for _, change := range changes {
			if matchesAny(r.paths, change.path) {
				problems = append(problems, "Changes protected path "+change.path+".")
			}
		}
		return boundedProblems(problems, 0)
	case "secrets":
		lines, err := in.addedLines(ctx)
		if err != nil {
			return []string{err.Error()}
		}
		var problems []string
		for _, added := range lines {
			if len(r.paths) > 0 && !matchesAny(r.paths, added.path) {
				continue
			}
			for _, builtin := range builtinSecretPatterns {
				if builtin.pattern.MatchString(added.text) {
					problems = append(problems, fmt.Sprintf("%s:%d adds what looks like a %s.", added.path, added.line, builtin.name))
				}
			}
			for _, custom := range r.patterns {
				if custom.MatchString(added.text) {
					problems = append(problems, fmt.Sprintf("%s:%d matches the secret pattern %s.", added.path, added.line, custom.String()))
				}
			}
		}
		return boundedProblems(problems, 0)
	case "commits":
		commits, err := in.log(ctx)
		if err != nil {
			return []string{err.Error()}
		}
		var problems []string
		for _, record := range commits {
			if len(record.commit.parents) > 1 {
				continue
			}
			subject := record.commit.subject()
			if r.MaxSubjectLength > 0 && utf8.RuneCountInString(subject) > r.MaxSubjectLength {
				problems = append(problems, fmt.Sprintf("Commit %s: the subject is %d characters; the limit is %d.",
					shortOid(record.oid), utf8.RuneCountInString(subject), r.MaxSubjectLength))
			} else if r.subject != nil && !r.subject.MatchString(subject) {
				problems = append(problems, fmt.Sprintf("Commit %s: %q does not match %s.",
					shortOid(record.oid), tailRunes(subject, 80), r.SubjectPattern))
			}
		}
		return boundedProblems(problems, 0)
	case "model":
		commits, err := in.log(ctx)
		if err != nil {
			return []string{err.Error()}
		}
		allowed := map[string]bool{}
		for _, model := range r.Models {
			allowed[model] = true
		}
		var problems []string
		for _, record := range commits {
			grant, ok := verifyAgentCommit(record.commit, in.routerKey)
			switch {
			case ok && allowed[grant.Model]:
			case ok:
				problems = append(problems, fmt.Sprintf("Commit %s was written by %s, which this rule does not allow.",
					shortOid(record.oid), grant.Model))
			case in.cleanMerge(ctx, record.commit):
			default:
				problems = append(problems, fmt.Sprintf("Commit %s has no verifiable model attestation.", shortOid(record.oid)))
			}
		}
		return boundedProblems(problems, 0)
	}
	return []string{"Unknown rule kind."}
}

type pullRuleSet struct {
	source string
	policy *PullRulesPolicy
	err    error
}

func policyErrorResult(source string, err error) PullRuleResult {
	where := "The repository's " + pullRulesPolicyPath
	if source == pullRuleSourceOrg {
		where = "The organization's pull request rules"
	}
	return PullRuleResult{
		ID: "policy", Title: "Rules can be read", Kind: "policy", Source: source,
		Enforcement: "block", Status: "failed",
		Problems:   []string{where + " could not be read: " + err.Error() + "."},
		Suggestion: "Fix the rules: until they parse, every merge they could cover stays locked.",
	}
}

func evaluatePullRules(ctx context.Context, in *pullRuleInput, sets []pullRuleSet, derived bool) PullRulesReport {
	report := PullRulesReport{Available: true, Branch: in.branch, Passed: true, Rules: []PullRuleResult{}}
	add := func(result PullRuleResult) {
		if result.Status != "passed" {
			report.Passed = false
			if result.Enforcement == "block" {
				report.Blocked = true
			}
		}
		report.Rules = append(report.Rules, result)
	}
	for _, set := range sets {
		if set.err != nil {
			add(policyErrorResult(set.source, set.err))
			continue
		}
		if set.policy == nil {
			continue
		}
		for i := range set.policy.Rules {
			rule := &set.policy.Rules[i]
			if !rule.appliesTo(in.branch) {
				continue
			}
			title := rule.Title
			if title == "" {
				title = defaultPullRuleTitle(rule.Kind)
			}
			result := PullRuleResult{
				ID: rule.ID, Title: title, Kind: rule.Kind, Source: set.source,
				Enforcement: rule.enforcement(), Status: "passed", Problems: []string{},
			}
			var problems []string
			if !derived {
				problems = []string{"Rules can only be checked on a branch-backed pull request."}
			} else if in.mergeBase == "" {
				problems = []string{"The branches share no history to compare."}
			} else {
				problems = rule.evaluate(ctx, in)
			}
			if len(problems) > 0 {
				result.Status = "failed"
				result.Problems = problems
				result.Suggestion = rule.suggestion()
			}
			add(result)
		}
	}
	return report
}

func readPullRulesPolicy(ctx context.Context, gitDir, baseOid string) (*PullRulesPolicy, error) {
	listing, err := pullRuleGit(ctx, gitDir, nil, 4096, "ls-tree", "-z", baseOid, "--", pullRulesPolicyPath)
	if err != nil {
		return nil, errors.New("the base commit could not be read")
	}
	entry := strings.TrimSuffix(string(listing), "\x00")
	if entry == "" {
		return nil, nil
	}
	fields := strings.SplitN(entry, "\t", 2)
	info := strings.Fields(fields[0])
	if len(fields) != 2 || len(info) != 3 || info[1] != "blob" || !gitObjectIDPattern.MatchString(info[2]) {
		return nil, errors.New("it is not a file")
	}
	raw, err := pullRuleGit(ctx, gitDir, nil, maxPullRulesPolicy, "cat-file", "blob", info[2])
	if err != nil {
		return nil, errors.New("it is too large")
	}
	return ParsePullRulesPolicy(raw)
}

func newPullRuleInput(ctx context.Context, gitDir, base, head, branch, routerKey string) *pullRuleInput {
	in := &pullRuleInput{gitDir: gitDir, base: base, head: head, branch: branch, routerKey: routerKey}
	if output, err := pullRuleGit(ctx, gitDir, nil, 256, "merge-base", base, head); err == nil {
		if oid := strings.TrimSpace(string(output)); gitObjectIDPattern.MatchString(oid) {
			in.mergeBase = oid
		}
	}
	return in
}

func (e *PullMutationExecutor) pullRulesSatisfied(ctx context.Context, gitDir string, request pullMutationRequest, meta *pullMetadata) bool {
	policy, err := readPullRulesPolicy(ctx, gitDir, request.ExpectedBaseOid)
	if err == nil && policy == nil {
		return true
	}
	in := newPullRuleInput(ctx, gitDir, request.ExpectedBaseOid, request.ExpectedHeadOid, meta.base, e.Gateway.RouterPublicKey)
	report := evaluatePullRules(ctx, in, []pullRuleSet{{source: pullRuleSourceRepository, policy: policy, err: err}}, meta.derived)
	return !report.Blocked
}

type pullRulesEvaluateRequest struct {
	SchemaVersion int    `json:"schemaVersion"`
	Type          string `json:"type"`
	Owner         string `json:"owner"`
	Repository    string `json:"repository"`
	Base          string `json:"base"`
	Head          string `json:"head"`
	Branch        string `json:"branch,omitempty"`
	PullNumber    int    `json:"pullNumber,omitempty"`
	OrgRules      string `json:"orgRules,omitempty"`
}

func (e *PullMutationExecutor) evaluateRulesRequest(raw []byte) (map[string]any, error) {
	var request pullRulesEvaluateRequest
	if err := decodeStrictJSON(raw, maxPullRulesRequest, &request); err != nil {
		return nil, err
	}
	request.Owner = strings.ToLower(strings.TrimSpace(request.Owner))
	request.Base = strings.ToLower(strings.TrimSpace(request.Base))
	request.Head = strings.ToLower(strings.TrimSpace(request.Head))
	if request.SchemaVersion != 1 || request.Type != pullRulesEvaluateType ||
		!nodePattern.MatchString(request.Owner) || !repositoryPattern.MatchString(request.Repository) ||
		!gitObjectIDPattern.MatchString(request.Base) || !gitObjectIDPattern.MatchString(request.Head) ||
		request.PullNumber < 0 || request.PullNumber > 999999999 ||
		(request.Branch != "" && validPullBranch(request.Branch) != request.Branch) ||
		len(request.OrgRules) > maxPullRulesPolicy {
		return nil, errors.New("invalid rules request")
	}
	repo, err := e.repositoryFor(pullMutationRequest{Owner: request.Owner, Repository: request.Repository})
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), pullGitTimeout)
	defer cancel()
	branch, derived := request.Branch, true
	if request.PullNumber > 0 {
		pulls, err := e.refValue(ctx, repo.GitDir, "refs/heads/"+pullMetadataBranch)
		if err != nil || pulls == "" {
			return map[string]any{"ok": true, "rules": PullRulesReport{Rules: []PullRuleResult{}}}, nil
		}
		meta, failure := e.readPullMetadata(ctx, repo.GitDir, pullMutationRequest{number: request.PullNumber, ExpectedPullsOid: pulls})
		if failure != "" {
			return map[string]any{"ok": true, "rules": PullRulesReport{Rules: []PullRuleResult{}}}, nil
		}
		branch, derived = meta.base, meta.derived
	}
	if branch == "" {
		return nil, errors.New("invalid rules request")
	}
	var sets []pullRuleSet
	if request.OrgRules != "" {
		policy, err := ParsePullRulesPolicy([]byte(request.OrgRules))
		sets = append(sets, pullRuleSet{source: pullRuleSourceOrg, policy: policy, err: err})
	}
	policy, err := readPullRulesPolicy(ctx, repo.GitDir, request.Base)
	sets = append(sets, pullRuleSet{source: pullRuleSourceRepository, policy: policy, err: err})
	in := newPullRuleInput(ctx, repo.GitDir, request.Base, request.Head, branch, e.Gateway.RouterPublicKey)
	return map[string]any{"ok": true, "rules": evaluatePullRules(ctx, in, sets, derived)}, nil
}
