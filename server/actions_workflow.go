package mirrornode

// Workflow files for the mirror Actions runner (actions.go).
//
// A workflow is a .forkmesh/<name>.yml file. This reads the same subset the
// desktop's ActionFile does, with the same meaning: `name`, `on` (a scalar, a
// list, or a mapping whose keys are the events), `on.push.paths`, `runs-on`,
// `env`, and steps taken from top-level `steps` and from every
// `jobs.<id>.steps`, flattened in file order. Only steps with a `run:` are
// kept; `uses:` steps name an action this runner does not have, so they are
// dropped rather than guessed at.
//
// The module has no dependencies, so the YAML below is a small indentation
// reader for that subset: block mappings and sequences, `[a, b]` flow lists,
// quoted and plain scalars, and `|` / `>` block scalars. Anything else is a
// parse error, never a partial workflow.

import (
	"errors"
	"fmt"
	"path"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	maxWorkflowBytes   = 256 << 10
	maxWorkflowSteps   = 64
	maxWorkflowEvents  = 8
	maxWorkflowLabels  = 8
	maxWorkflowDepth   = 16
	maxWorkflowInputs  = 25
	maxWorkflowCrons   = 8
	maxWorkflowMinutes = 1440
)

var (
	workflowPathPattern = regexp.MustCompile(`^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$`)
	workflowVarPattern  = regexp.MustCompile(`\$\{\{\s*vars\.[A-Za-z_][A-Za-z0-9_]*\s*\}\}`)
	workflowInputName   = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_-]{0,99}$`)
	workflowCronField   = regexp.MustCompile(`^(?:\*|[0-9A-Za-z]{1,3}(?:-[0-9A-Za-z]{1,3})?)(?:/[0-9]{1,2})?(?:,(?:\*|[0-9A-Za-z]{1,3}(?:-[0-9A-Za-z]{1,3})?)(?:/[0-9]{1,2})?)*$`)
)

type workflowStep struct {
	Name             string
	Run              string
	Job              string
	JobIf            string
	If               string
	Env              [][2]string
	WorkingDirectory string
	TimeoutMinutes   int
}

type workflowInput struct {
	Name        string   `json:"name"`
	Description string   `json:"description"`
	Required    bool     `json:"required"`
	Default     string   `json:"default"`
	Type        string   `json:"type"`
	Options     []string `json:"options"`
}

type workflow struct {
	Path           string
	Name           string
	Events         []string
	PushPaths      []string
	PushBranches   []string
	Schedules      []string
	Inputs         []workflowInput
	IssueTypes     []string
	CommentTypes   []string
	PullBranches   []string
	ContentsWrite  bool
	TimeoutMinutes int
	RunsOn         []string
	// Names of other workflows that must succeed for the same commit first.
	Needs []string
	Env   [][2]string
	Steps []workflowStep
}

func (w *workflow) on(event string) bool {
	for _, name := range w.Events {
		if name == event {
			return true
		}
	}
	return false
}

func (w *workflow) manual() bool { return w.on("workflow_dispatch") }

// matchesPushPaths reports whether a push touching `changed` should run this
// workflow. No `on.push.paths` filter means every push does.
func (w *workflow) matchesPushPaths(changed []string) bool {
	if len(w.PushPaths) == 0 {
		return true
	}
	for _, file := range changed {
		for _, pattern := range w.PushPaths {
			if workflowPathMatches(pattern, file) {
				return true
			}
		}
	}
	return false
}

// matchesPushBranch applies `on.push.branches` to a short branch name.
func (w *workflow) matchesPullBranch(branch string) bool {
	for _, pattern := range w.PullBranches {
		if workflowPathMatches(pattern, branch) {
			return true
		}
	}
	return false
}

func (w *workflow) matchesPushBranch(branch string) bool {
	for _, pattern := range w.PushBranches {
		if workflowPathMatches(pattern, branch) {
			return true
		}
	}
	return false
}

// workflowPathMatches implements the two wildcards the desktop honours: `*`
// within one path segment and `**` across segments.
func workflowPathMatches(pattern, file string) bool {
	pattern = strings.TrimPrefix(pattern, "./")
	if pattern == "" {
		return false
	}
	if !strings.Contains(pattern, "**") {
		ok, err := path.Match(pattern, file)
		return err == nil && ok
	}
	var expression strings.Builder
	expression.WriteString("^")
	for i := 0; i < len(pattern); i++ {
		switch {
		case strings.HasPrefix(pattern[i:], "**/"):
			expression.WriteString("(?:.*/)?")
			i += 2
		case strings.HasPrefix(pattern[i:], "**"):
			expression.WriteString(".*")
			i++
		case pattern[i] == '*':
			expression.WriteString("[^/]*")
		case pattern[i] == '?':
			expression.WriteString("[^/]")
		default:
			expression.WriteString(regexp.QuoteMeta(string(pattern[i])))
		}
	}
	expression.WriteString("$")
	compiled, err := regexp.Compile(expression.String())
	return err == nil && compiled.MatchString(file)
}

// runsOnThisNode applies `runs-on`: no labels means any node.
func (w *workflow) runsOnThisNode(labels []string) bool {
	if len(w.RunsOn) == 0 {
		return true
	}
	for _, wanted := range w.RunsOn {
		for _, label := range labels {
			if strings.EqualFold(wanted, label) {
				return true
			}
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// YAML subset.

type yamlKind int

const (
	yamlScalar yamlKind = iota
	yamlMap
	yamlSeq
)

type yamlNode struct {
	kind   yamlKind
	scalar string
	keys   []string
	values []*yamlNode
	items  []*yamlNode
}

func (n *yamlNode) get(key string) *yamlNode {
	if n == nil || n.kind != yamlMap {
		return nil
	}
	for i, name := range n.keys {
		if name == key {
			return n.values[i]
		}
	}
	return nil
}

func (n *yamlNode) text() string {
	if n == nil || n.kind != yamlScalar {
		return ""
	}
	return n.scalar
}

// strings returns a scalar as one entry, a sequence of scalars as many.
func (n *yamlNode) strings() []string {
	if n == nil {
		return nil
	}
	if n.kind == yamlScalar {
		if n.scalar == "" {
			return nil
		}
		return []string{n.scalar}
	}
	var out []string
	if n.kind == yamlSeq {
		for _, item := range n.items {
			if item.kind == yamlScalar && item.scalar != "" {
				out = append(out, item.scalar)
			}
		}
	}
	return out
}

type yamlLine struct {
	indent int
	text   string // comment-stripped, right-trimmed; "" for a blank line
	raw    string // original line, for block scalars
}

type yamlParser struct {
	lines []yamlLine
	pos   int
}

var errWorkflowSyntax = errors.New("workflow file is not in the supported YAML subset")

// fail reports where the reader stopped. A line number is safe to publish; the
// line's text is not, so it is never included.
func (p *yamlParser) fail() error {
	line := p.pos + 1
	if line > len(p.lines) {
		line = len(p.lines)
	}
	return fmt.Errorf("%w (line %d)", errWorkflowSyntax, line)
}

func parseYAMLSubset(content string) (*yamlNode, error) {
	parser := &yamlParser{}
	for _, raw := range strings.Split(strings.ReplaceAll(content, "\r\n", "\n"), "\n") {
		stripped := strings.TrimRight(stripYAMLComment(raw), " ")
		indent := len(stripped) - len(strings.TrimLeft(stripped, " "))
		parser.lines = append(parser.lines, yamlLine{
			indent: indent, text: strings.TrimLeft(stripped, " "), raw: raw})
	}
	parser.skipBlank()
	if parser.pos >= len(parser.lines) {
		return nil, errWorkflowSyntax
	}
	if parser.lines[parser.pos].text == "---" {
		parser.pos++
		parser.skipBlank()
	}
	if parser.pos >= len(parser.lines) {
		return nil, errWorkflowSyntax
	}
	node, err := parser.block(parser.lines[parser.pos].indent, 0)
	if err != nil {
		return nil, err
	}
	parser.skipBlank()
	if parser.pos != len(parser.lines) {
		return nil, errWorkflowSyntax
	}
	return node, nil
}

// stripYAMLComment removes a `#` comment that is not inside quotes. A `#`
// only starts a comment at the beginning of the line or after whitespace.
func stripYAMLComment(line string) string {
	inSingle, inDouble := false, false
	for i := 0; i < len(line); i++ {
		switch c := line[i]; {
		case c == '\\' && inDouble:
			i++
		case c == '\'' && !inDouble:
			inSingle = !inSingle
		case c == '"' && !inSingle:
			inDouble = !inDouble
		case c == '#' && !inSingle && !inDouble:
			if i == 0 || line[i-1] == ' ' || line[i-1] == '\t' {
				return line[:i]
			}
		}
	}
	return line
}

func (p *yamlParser) skipBlank() {
	for p.pos < len(p.lines) && p.lines[p.pos].text == "" {
		p.pos++
	}
}

func (p *yamlParser) block(indent, depth int) (*yamlNode, error) {
	if depth > maxWorkflowDepth {
		return nil, p.fail()
	}
	p.skipBlank()
	if p.pos >= len(p.lines) || p.lines[p.pos].indent != indent {
		return nil, p.fail()
	}
	if isYAMLSeqItem(p.lines[p.pos].text) {
		return p.sequence(indent, depth)
	}
	return p.mapping(indent, depth)
}

func isYAMLSeqItem(text string) bool { return text == "-" || strings.HasPrefix(text, "- ") }

func (p *yamlParser) sequence(indent, depth int) (*yamlNode, error) {
	node := &yamlNode{kind: yamlSeq}
	for {
		p.skipBlank()
		if p.pos >= len(p.lines) || p.lines[p.pos].indent != indent ||
			!isYAMLSeqItem(p.lines[p.pos].text) {
			break
		}
		line := p.lines[p.pos]
		rest := strings.TrimLeft(strings.TrimPrefix(line.text, "-"), " ")
		if rest == "" {
			p.pos++
			p.skipBlank()
			if p.pos >= len(p.lines) || p.lines[p.pos].indent <= indent {
				node.items = append(node.items, &yamlNode{kind: yamlScalar})
				continue
			}
			child, err := p.block(p.lines[p.pos].indent, depth+1)
			if err != nil {
				return nil, err
			}
			node.items = append(node.items, child)
			continue
		}
		if _, _, ok := splitYAMLKey(rest); ok {
			// "- key: value": the item is a mapping whose first entry sits on
			// this line, indented to where the key begins.
			itemIndent := indent + (len(line.text) - len(rest))
			p.lines[p.pos] = yamlLine{indent: itemIndent, text: rest,
				raw: strings.Repeat(" ", itemIndent) + rest}
			child, err := p.mapping(itemIndent, depth+1)
			if err != nil {
				return nil, err
			}
			node.items = append(node.items, child)
			continue
		}
		value, err := yamlInlineValue(rest)
		if err != nil {
			return nil, err
		}
		p.pos++
		node.items = append(node.items, value)
	}
	if len(node.items) == 0 {
		return nil, p.fail()
	}
	return node, nil
}

func (p *yamlParser) mapping(indent, depth int) (*yamlNode, error) {
	if depth > maxWorkflowDepth {
		return nil, p.fail()
	}
	node := &yamlNode{kind: yamlMap}
	for {
		p.skipBlank()
		if p.pos >= len(p.lines) || p.lines[p.pos].indent != indent ||
			isYAMLSeqItem(p.lines[p.pos].text) {
			break
		}
		key, rest, ok := splitYAMLKey(p.lines[p.pos].text)
		if !ok {
			return nil, p.fail()
		}
		for _, existing := range node.keys {
			if existing == key {
				return nil, p.fail()
			}
		}
		p.pos++
		var value *yamlNode
		var err error
		switch {
		case rest == "":
			p.skipBlank()
			switch {
			case p.pos < len(p.lines) && p.lines[p.pos].indent > indent:
				value, err = p.block(p.lines[p.pos].indent, depth+1)
			case p.pos < len(p.lines) && p.lines[p.pos].indent == indent &&
				isYAMLSeqItem(p.lines[p.pos].text):
				// A sequence may sit at its key's own indentation.
				value, err = p.sequence(indent, depth+1)
			default:
				value = &yamlNode{kind: yamlScalar}
			}
		case isYAMLBlockScalar(rest):
			value = &yamlNode{kind: yamlScalar, scalar: p.blockScalar(indent, rest)}
		default:
			value, err = yamlInlineValue(rest)
		}
		if err != nil {
			return nil, err
		}
		node.keys = append(node.keys, key)
		node.values = append(node.values, value)
	}
	if len(node.keys) == 0 {
		return nil, p.fail()
	}
	return node, nil
}

// splitYAMLKey splits "key: rest" / "key:". The colon must end the text or be
// followed by a space, so "http://x" and "12:30" stay scalars.
func splitYAMLKey(text string) (string, string, bool) {
	// YAML forbids tab indentation. Tabs inside a block scalar never reach
	// here: blockScalar consumes those lines whole.
	if text == "" || text[0] == '[' || text[0] == '{' || text[0] == '\t' {
		return "", "", false
	}
	if text[0] == '"' || text[0] == '\'' {
		quote := text[0]
		end := strings.IndexByte(text[1:], quote)
		if end < 0 {
			return "", "", false
		}
		after := text[end+2:]
		if after != ":" && !strings.HasPrefix(after, ": ") {
			return "", "", false
		}
		return text[1 : end+1], strings.TrimLeft(strings.TrimPrefix(after, ":"), " "), true
	}
	for i := 0; i < len(text); i++ {
		if text[i] != ':' {
			continue
		}
		if i+1 == len(text) || text[i+1] == ' ' {
			key := strings.TrimSpace(text[:i])
			if key == "" {
				return "", "", false
			}
			return key, strings.TrimLeft(text[i+1:], " "), true
		}
	}
	return "", "", false
}

func isYAMLBlockScalar(rest string) bool {
	switch rest {
	case "|", "|-", "|+", ">", ">-", ">+":
		return true
	}
	return false
}

// blockScalar consumes the lines more indented than its key. It reads the
// original lines, so `#` and trailing spaces inside a command survive.
func (p *yamlParser) blockScalar(keyIndent int, header string) string {
	var body []string
	blockIndent := -1
	for p.pos < len(p.lines) {
		raw := p.lines[p.pos].raw
		if strings.TrimSpace(raw) == "" {
			body = append(body, "")
			p.pos++
			continue
		}
		indent := len(raw) - len(strings.TrimLeft(raw, " "))
		if indent <= keyIndent {
			break
		}
		if blockIndent < 0 {
			blockIndent = indent
		}
		if indent < blockIndent {
			break
		}
		body = append(body, strings.TrimRight(raw[blockIndent:], "\r"))
		p.pos++
	}
	for len(body) > 0 && body[len(body)-1] == "" {
		body = body[:len(body)-1]
	}
	if len(body) == 0 {
		return ""
	}
	if strings.HasPrefix(header, ">") {
		return strings.Join(body, " ")
	}
	text := strings.Join(body, "\n")
	if header == "|-" {
		return text
	}
	return text + "\n"
}

func yamlInlineValue(text string) (*yamlNode, error) {
	text = strings.TrimSpace(text)
	if strings.HasPrefix(text, "[") {
		if !strings.HasSuffix(text, "]") {
			return nil, errWorkflowSyntax
		}
		node := &yamlNode{kind: yamlSeq}
		inner := strings.TrimSpace(text[1 : len(text)-1])
		if inner == "" {
			return node, nil
		}
		for _, part := range splitYAMLFlow(inner) {
			scalar, err := yamlScalarValue(strings.TrimSpace(part))
			if err != nil {
				return nil, err
			}
			node.items = append(node.items, &yamlNode{kind: yamlScalar, scalar: scalar})
		}
		return node, nil
	}
	if strings.HasPrefix(text, "{") {
		if text == "{}" {
			return &yamlNode{kind: yamlScalar}, nil
		}
		return nil, errWorkflowSyntax
	}
	scalar, err := yamlScalarValue(text)
	if err != nil {
		return nil, err
	}
	return &yamlNode{kind: yamlScalar, scalar: scalar}, nil
}

func splitYAMLFlow(inner string) []string {
	var parts []string
	var current strings.Builder
	var quote byte
	for i := 0; i < len(inner); i++ {
		c := inner[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			}
			current.WriteByte(c)
		case c == '"' || c == '\'':
			quote = c
			current.WriteByte(c)
		case c == ',':
			parts = append(parts, current.String())
			current.Reset()
		default:
			current.WriteByte(c)
		}
	}
	return append(parts, current.String())
}

func yamlScalarValue(text string) (string, error) {
	if len(text) >= 2 && text[0] == '\'' && text[len(text)-1] == '\'' {
		return strings.ReplaceAll(text[1:len(text)-1], "''", "'"), nil
	}
	if len(text) >= 2 && text[0] == '"' && text[len(text)-1] == '"' {
		var out strings.Builder
		body := text[1 : len(text)-1]
		for i := 0; i < len(body); i++ {
			if body[i] != '\\' || i+1 == len(body) {
				out.WriteByte(body[i])
				continue
			}
			i++
			switch body[i] {
			case 'n':
				out.WriteByte('\n')
			case 't':
				out.WriteByte('\t')
			case '"', '\\', '/':
				out.WriteByte(body[i])
			default:
				return "", errWorkflowSyntax
			}
		}
		return out.String(), nil
	}
	if text != "" && (text[0] == '"' || text[0] == '\'' || text[0] == '&' ||
		text[0] == '*' || text[0] == '!' || text[0] == '%' || text[0] == '@') {
		return "", errWorkflowSyntax
	}
	if text == "~" || text == "null" {
		return "", nil
	}
	return text, nil
}

// ---------------------------------------------------------------------------

func singleLine(value string, limit int) string {
	fields := strings.Fields(strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return ' '
		}
		return r
	}, value))
	return boundedRunes(strings.Join(fields, " "), limit)
}

// parseWorkflow reads one workflow file. The error is a short reason that is
// safe to publish: it never quotes the file.
func parseWorkflow(filePath, content string) (*workflow, error) {
	if !workflowPathPattern.MatchString(filePath) {
		return nil, errors.New("workflow path is invalid")
	}
	if len(content) == 0 || len(content) > maxWorkflowBytes || !utf8.ValidString(content) ||
		strings.ContainsRune(content, 0) {
		return nil, errors.New("workflow file is empty, too large, or not UTF-8")
	}
	root, err := parseYAMLSubset(content)
	if err != nil {
		return nil, err
	}
	if root.kind != yamlMap {
		return nil, errors.New("workflow file is not a mapping")
	}
	result := &workflow{Path: filePath}
	result.Name = singleLine(root.get("name").text(), 160)
	if result.Name == "" {
		result.Name = strings.TrimSuffix(strings.TrimSuffix(path.Base(filePath), ".yml"), ".yaml")
	}

	// "on" is a YAML 1.1 boolean, so a strict loader would hand back `true`;
	// here it is just a key, which is what every workflow author means.
	switch on := root.get("on"); {
	case on == nil:
	case on.kind == yamlMap:
		result.Events = append(result.Events, on.keys...)
		if push := on.get("push"); push != nil && push.kind == yamlMap {
			result.PushPaths = push.get("paths").strings()
			result.PushBranches = push.get("branches").strings()
		}
		if schedule := on.get("schedule"); schedule != nil && schedule.kind == yamlSeq {
			for _, item := range schedule.items {
				if cron := normalizeWorkflowCron(item.get("cron").text()); cron != "" &&
					len(result.Schedules) < maxWorkflowCrons {
					result.Schedules = append(result.Schedules, cron)
				}
			}
		}
		inputs, err := workflowInputs(on.get("workflow_dispatch").get("inputs"))
		if err != nil {
			return nil, err
		}
		result.Inputs = inputs
		result.IssueTypes = on.get("issues").get("types").strings()
		result.CommentTypes = on.get("issue_comment").get("types").strings()
		result.PullBranches = on.get("pull_request").get("branches").strings()
	default:
		result.Events = on.strings()
	}
	if len(result.Events) > maxWorkflowEvents {
		return nil, errors.New("workflow names too many events")
	}
	for i, event := range result.Events {
		result.Events[i] = singleLine(event, 40)
	}

	result.RunsOn = root.get("runs-on").strings()
	result.Env = workflowEnv(root.get("env"))
	result.ContentsWrite = workflowContentsWrite(root.get("permissions"))
	baseDir, err := workflowWorkingDirectory(root.get("defaults").get("run").get("working-directory"))
	if err != nil {
		return nil, err
	}
	var collectErr error
	collect := func(steps *yamlNode, job, jobIf, jobDir string, jobEnv [][2]string) {
		if steps == nil || steps.kind != yamlSeq {
			return
		}
		for _, step := range steps.items {
			command := step.get("run").text()
			if strings.TrimSpace(command) == "" {
				continue
			}
			name := singleLine(step.get("name").text(), 120)
			if name == "" {
				name = singleLine(strings.SplitN(strings.TrimSpace(command), "\n", 2)[0], 120)
			}
			dir, err := workflowWorkingDirectory(step.get("working-directory"))
			if dir == "" {
				dir = jobDir
			}
			timeout, timeoutErr := workflowMinutes(step.get("timeout-minutes"))
			if err == nil {
				err = timeoutErr
			}
			if err != nil && collectErr == nil {
				collectErr = err
			}
			result.Steps = append(result.Steps, workflowStep{Name: name, Run: command, Job: job,
				JobIf: jobIf, If: strings.TrimSpace(step.get("if").text()),
				Env: mergeWorkflowEnv(jobEnv, workflowEnv(step.get("env"))), WorkingDirectory: dir,
				TimeoutMinutes: timeout})
		}
	}
	collect(root.get("steps"), "", "", baseDir, nil)
	needs := root.get("needs").strings()
	if jobs := root.get("jobs"); jobs != nil && jobs.kind == yamlMap {
		for jobIndex, job := range jobs.values {
			// A job may need another job in this file; the jobs are already
			// flattened in order, so only a name from outside the file is a
			// dependency on another workflow.
			for _, needed := range job.get("needs").strings() {
				if jobs.get(needed) == nil {
					needs = append(needs, needed)
				}
			}
			if len(result.RunsOn) == 0 {
				result.RunsOn = job.get("runs-on").strings()
			}
			if workflowContentsWrite(job.get("permissions")) {
				result.ContentsWrite = true
			}
			jobDir, err := workflowWorkingDirectory(job.get("defaults").get("run").get("working-directory"))
			if err != nil {
				return nil, err
			}
			if jobDir == "" {
				jobDir = baseDir
			}
			minutes, err := workflowMinutes(job.get("timeout-minutes"))
			if err != nil {
				return nil, err
			}
			result.TimeoutMinutes = max(result.TimeoutMinutes, minutes)
			collect(job.get("steps"), jobs.keys[jobIndex], strings.TrimSpace(job.get("if").text()),
				jobDir, workflowEnv(job.get("env")))
		}
	}
	if collectErr != nil {
		return nil, collectErr
	}
	if len(result.RunsOn) > maxWorkflowLabels {
		result.RunsOn = result.RunsOn[:maxWorkflowLabels]
	}
	for i, label := range result.RunsOn {
		result.RunsOn[i] = singleLine(label, 64)
	}
	for _, needed := range needs {
		if name := singleLine(needed, 160); name != "" && len(result.Needs) < maxWorkflowLabels {
			result.Needs = append(result.Needs, name)
		}
	}
	if len(result.Steps) == 0 {
		return nil, errors.New("workflow has no runnable steps")
	}
	if len(result.Steps) > maxWorkflowSteps {
		return nil, errors.New("workflow has too many steps")
	}
	return result, nil
}

func workflowEnv(node *yamlNode) [][2]string {
	var out [][2]string
	if node == nil || node.kind != yamlMap {
		return nil
	}
	for i, key := range node.keys {
		if node.values[i].kind == yamlScalar {
			out = append(out, [2]string{key, node.values[i].scalar})
		}
	}
	return out
}

func mergeWorkflowEnv(layers ...[][2]string) [][2]string {
	var out [][2]string
	for _, layer := range layers {
		for _, pair := range layer {
			replaced := false
			for i := range out {
				if out[i][0] == pair[0] {
					out[i][1], replaced = pair[1], true
				}
			}
			if !replaced {
				out = append(out, pair)
			}
		}
	}
	return out
}

func workflowContentsWrite(node *yamlNode) bool {
	if node == nil {
		return false
	}
	if node.kind == yamlScalar {
		return node.scalar == "write-all"
	}
	return node.get("contents").text() == "write"
}

func workflowWorkingDirectory(node *yamlNode) (string, error) {
	dir := strings.TrimSpace(node.text())
	if dir == "" {
		return "", nil
	}
	if strings.HasPrefix(dir, "/") || strings.Contains(dir, "..") || strings.ContainsAny(dir, "\\\x00") {
		return "", errors.New("workflow working-directory must stay inside the checkout")
	}
	dir = path.Clean(dir)
	if dir == "." {
		return "", nil
	}
	return dir, nil
}

func workflowMinutes(node *yamlNode) (int, error) {
	text := strings.TrimSpace(node.text())
	if text == "" {
		return 0, nil
	}
	minutes, err := strconv.Atoi(text)
	if err != nil || minutes < 1 || minutes > maxWorkflowMinutes {
		return 0, errors.New("workflow timeout-minutes is not a whole number of minutes")
	}
	return minutes, nil
}

func normalizeWorkflowCron(cron string) string {
	fields := strings.Fields(cron)
	switch {
	case len(fields) == 1:
		switch fields[0] {
		case "@hourly", "@daily", "@weekly", "@monthly":
			return fields[0]
		}
		return ""
	case len(fields) != 5:
		return ""
	}
	for _, field := range fields {
		if !workflowCronField.MatchString(field) {
			return ""
		}
	}
	return strings.Join(fields, " ")
}

func workflowInputs(node *yamlNode) ([]workflowInput, error) {
	if node == nil || node.kind != yamlMap {
		return nil, nil
	}
	if len(node.keys) > maxWorkflowInputs {
		return nil, errors.New("workflow has too many inputs")
	}
	var out []workflowInput
	for i, name := range node.keys {
		if !workflowInputName.MatchString(name) {
			return nil, errors.New("workflow input name is invalid")
		}
		spec := node.values[i]
		input := workflowInput{Name: name, Description: singleLine(spec.get("description").text(), 240),
			Default: boundedRunes(spec.get("default").text(), 1024), Type: spec.get("type").text()}
		switch spec.get("required").text() {
		case "", "false", "False":
		case "true", "True":
			input.Required = true
		default:
			return nil, errors.New("workflow input required must be true or false")
		}
		switch input.Type {
		case "":
			input.Type = "string"
		case "string", "boolean":
		case "choice":
			for _, option := range spec.get("options").strings() {
				input.Options = append(input.Options, boundedRunes(option, 200))
			}
			if len(input.Options) == 0 || len(input.Options) > 64 {
				return nil, errors.New("workflow choice input needs between 1 and 64 options")
			}
		default:
			return nil, errors.New("workflow input type must be string, choice or boolean")
		}
		out = append(out, input)
	}
	return out, nil
}

// substituteWorkflowVars blanks `${{ vars.NAME }}`. A mirror holds no Actions
// variables: it is somebody else's machine, and a secret placed there would
// be readable by its operator. Workflows that need one belong on the owner's
// own desktop node.
func substituteWorkflowVars(command string) (string, bool) {
	if !workflowVarPattern.MatchString(command) {
		return command, false
	}
	return workflowVarPattern.ReplaceAllString(command, ""), true
}
