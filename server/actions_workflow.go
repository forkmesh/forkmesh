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
	"strings"
	"unicode/utf8"
)

const (
	maxWorkflowBytes  = 256 << 10
	maxWorkflowSteps  = 64
	maxWorkflowEvents = 8
	maxWorkflowLabels = 8
	maxWorkflowDepth  = 16
)

var (
	workflowPathPattern = regexp.MustCompile(`^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$`)
	workflowVarPattern  = regexp.MustCompile(`\$\{\{\s*vars\.[A-Za-z_][A-Za-z0-9_]*\s*\}\}`)
)

type workflowStep struct {
	Name string
	Run  string
}

type workflow struct {
	Path         string
	Name         string
	Events       []string
	PushPaths    []string
	PushBranches []string
	RunsOn       []string
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
	if env := root.get("env"); env != nil && env.kind == yamlMap {
		for i, key := range env.keys {
			if env.values[i].kind == yamlScalar {
				result.Env = append(result.Env, [2]string{key, env.values[i].scalar})
			}
		}
	}
	collect := func(steps *yamlNode) {
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
			result.Steps = append(result.Steps, workflowStep{Name: name, Run: command})
		}
	}
	collect(root.get("steps"))
	needs := root.get("needs").strings()
	if jobs := root.get("jobs"); jobs != nil && jobs.kind == yamlMap {
		for _, job := range jobs.values {
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
			collect(job.get("steps"))
		}
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
