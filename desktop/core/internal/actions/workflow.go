package actions

import (
	"os"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
)

type Step struct {
	Name string `json:"name"`
	Run  string `json:"run"`
}

type Workflow struct {
	Path             string            `json:"path"`
	Content          string            `json:"content,omitempty"`
	Name             string            `json:"name"`
	On               []string          `json:"on"`
	PushChangedPaths []string          `json:"pushChangedPaths"`
	Env              map[string]string `json:"env"`
	RunsOn           []string          `json:"runsOn"`
	Needs            []string          `json:"needs"`
	Steps            []Step            `json:"steps"`
	Checkout         bool              `json:"checkout"`
	FetchDepth       int               `json:"fetchDepth"`
	Valid            bool              `json:"valid"`
	Error            string            `json:"error,omitempty"`
}

func (w Workflow) TriggersOnPush() bool        { return containsString(w.On, "push") }
func (w Workflow) TriggersOnRelease() bool     { return containsString(w.On, "release") }
func (w Workflow) TriggersOnPullRequest() bool { return containsString(w.On, "pull_request") }
func (w Workflow) AllowsManualRun() bool       { return containsString(w.On, "workflow_dispatch") }

type nodeType int

const (
	nodeNull nodeType = iota
	nodeScalar
	nodeMap
	nodeSeq
)

type node struct {
	kind   nodeType
	scalar string
	keys   map[string]*node
	seq    []*node
}

func (n *node) child(key string) *node {
	if n == nil || n.keys == nil {
		return nil
	}
	return n.keys[key]
}

func (n *node) sortedKeys() []string {
	keys := make([]string, 0, len(n.keys))
	for key := range n.keys {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool { return qtKeyLess(keys[i], keys[j]) })
	return keys
}

type line struct {
	indent int
	text   string
}

func splitLines(content string) []line {
	var out []line
	for _, raw := range strings.Split(content, "\n") {
		raw = strings.ReplaceAll(raw, "\t", "    ")
		for strings.HasSuffix(raw, "\r") {
			raw = raw[:len(raw)-1]
		}
		trimmed := qtTrim(raw)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}
		indent := 0
		for indent < len(raw) && raw[indent] == ' ' {
			indent++
		}
		out = append(out, line{indent: indent, text: raw[indent:]})
	}
	return out
}

func unquote(value string) string {
	value = qtTrim(value)
	if len(value) >= 2 && ((value[0] == '"' && value[len(value)-1] == '"') || (value[0] == '\'' && value[len(value)-1] == '\'')) {
		value = value[1 : len(value)-1]
	}
	return value
}

func parseFlowSeq(inner string) *node {
	out := &node{kind: nodeSeq}
	for _, part := range strings.Split(inner, ",") {
		value := unquote(part)
		if value == "" {
			continue
		}
		out.seq = append(out.seq, &node{kind: nodeScalar, scalar: value})
	}
	return out
}

func captureBlockScalar(lines []line, pos *int, parentIndent int) string {
	var block []string
	blockIndent := -1
	for *pos < len(lines) && lines[*pos].indent > parentIndent {
		l := lines[*pos]
		if blockIndent < 0 {
			blockIndent = l.indent
		}
		strip := blockIndent
		if l.indent < strip {
			strip = l.indent
		}
		block = append(block, strings.Repeat(" ", l.indent-strip)+l.text)
		*pos++
	}
	return strings.Join(block, "\n")
}

func parseNestedValue(lines []line, pos *int, parentIndent int) *node {
	if *pos >= len(lines) || lines[*pos].indent <= parentIndent {
		return &node{kind: nodeNull}
	}
	return parseBlock(lines, pos, lines[*pos].indent)
}

func parseMapEntry(lines []line, pos *int, indent int, into *node) {
	text := lines[*pos].text
	colon := strings.IndexByte(text, ':')
	if colon < 0 {
		*pos++
		return
	}
	key := unquote(text[:colon])
	rest := qtTrim(text[colon+1:])
	*pos++
	var value *node
	switch {
	case rest == "|" || rest == "|-" || rest == ">" || rest == ">-":
		value = &node{kind: nodeScalar, scalar: captureBlockScalar(lines, pos, indent)}
	case strings.HasPrefix(rest, "[") && strings.HasSuffix(rest, "]"):
		value = parseFlowSeq(rest[1 : len(rest)-1])
	case rest != "":
		value = &node{kind: nodeScalar, scalar: unquote(rest)}
	default:
		value = parseNestedValue(lines, pos, indent)
	}
	if into.keys == nil {
		into.keys = map[string]*node{}
	}
	into.keys[key] = value
}

func indexOfRuneUTF16(text string, target rune) int {
	units := 0
	for _, r := range text {
		if r == target {
			return units
		}
		if r > 0xFFFF {
			units += 2
		} else {
			units++
		}
	}
	return -1
}

func parseBlock(lines []line, pos *int, indent int) *node {
	first := lines[*pos].text
	out := &node{kind: nodeMap, keys: map[string]*node{}}
	if strings.HasPrefix(first, "- ") || first == "-" {
		out = &node{kind: nodeSeq}
	}
	for *pos < len(lines) && lines[*pos].indent == indent {
		current := lines[*pos]
		if out.kind == nodeSeq {
			if !strings.HasPrefix(current.text, "-") {
				break
			}
			after := qtTrim(current.text[1:])
			if strings.Contains(after, ":") && !strings.HasPrefix(after, "[") {
				firstRune := []rune(after)[0]
				dashCols := indexOfRuneUTF16(current.text, firstRune)
				synthetic := line{indent: indent + dashCols, text: after}
				original := lines[*pos]
				lines[*pos] = synthetic
				sub := *pos
				item := parseBlock(lines, &sub, synthetic.indent)
				lines[*pos] = original
				out.seq = append(out.seq, item)
				*pos = sub
			} else if after != "" {
				out.seq = append(out.seq, &node{kind: nodeScalar, scalar: unquote(after)})
				*pos++
			} else {
				*pos++
				out.seq = append(out.seq, parseNestedValue(lines, pos, indent))
			}
		} else {
			parseMapEntry(lines, pos, indent, out)
		}
	}
	return out
}

func scalarOrList(n *node) []string {
	var out []string
	if n == nil {
		return out
	}
	switch n.kind {
	case nodeScalar:
		out = append(out, qtTrim(n.scalar))
	case nodeSeq:
		for _, item := range n.seq {
			if item.kind == nodeScalar {
				out = append(out, qtTrim(item.scalar))
			}
		}
	}
	return out
}

func eventNames(n *node) []string {
	var out []string
	if n == nil {
		return out
	}
	if n.kind == nodeMap {
		for _, key := range n.sortedKeys() {
			event := qtTrim(key)
			if event != "" && !containsString(out, event) {
				out = append(out, event)
			}
		}
		return out
	}
	for _, event := range scalarOrList(n) {
		if event != "" && !containsString(out, event) {
			out = append(out, event)
		}
	}
	return out
}

func collectSteps(n *node, steps []Step) []Step {
	if n == nil || n.kind != nodeSeq {
		return steps
	}
	for _, item := range n.seq {
		if item.kind != nodeMap {
			continue
		}
		var step Step
		if name := item.child("name"); name != nil {
			step.Name = qtTrim(name.scalar)
		}
		if run := item.child("run"); run != nil {
			step.Run = run.scalar
		}
		if step.Run != "" {
			steps = append(steps, step)
		}
	}
	return steps
}

func qtFileName(rel string) string {
	if index := strings.LastIndexByte(rel, '/'); index >= 0 {
		return rel[index+1:]
	}
	return rel
}

func qtCompleteBaseName(rel string) string {
	name := qtFileName(rel)
	if index := strings.LastIndexByte(name, '.'); index >= 0 {
		return name[:index]
	}
	return name
}

func Parse(relPath, content string) Workflow {
	wf := Workflow{Path: relPath, Content: content, Name: qtFileName(relPath), Checkout: true, FetchDepth: 2, Env: map[string]string{}}
	lines := splitLines(content)
	if len(lines) == 0 {
		wf.Error = "empty workflow file"
		return wf
	}
	pos := 0
	root := parseBlock(lines, &pos, lines[0].indent)
	if root.kind != nodeMap {
		wf.Error = "workflow root must be a mapping"
		return wf
	}
	if name := root.child("name"); name != nil && qtTrim(name.scalar) != "" {
		wf.Name = qtTrim(name.scalar)
	}
	onNode := root.child("on")
	wf.On = eventNames(onNode)
	if onNode != nil && onNode.kind == nodeMap {
		for _, key := range onNode.sortedKeys() {
			if qtTrim(key) != "push" {
				continue
			}
			if paths := onNode.keys[key].child("paths"); paths != nil {
				for _, pattern := range scalarOrList(paths) {
					normalized := qtTrim(pattern)
					if normalized != "" && !containsString(wf.PushChangedPaths, normalized) {
						wf.PushChangedPaths = append(wf.PushChangedPaths, normalized)
					}
				}
			}
		}
	}
	addRunsOn := func(n *node) {
		for _, label := range scalarOrList(n) {
			normalized := strings.ToLower(qtTrim(label))
			if normalized != "" && !containsString(wf.RunsOn, normalized) {
				wf.RunsOn = append(wf.RunsOn, normalized)
			}
		}
	}
	addRunsOn(root.child("runs-on"))
	if checkout := root.child("checkout"); checkout != nil {
		value := strings.ToLower(qtTrim(checkout.scalar))
		wf.Checkout = value != "false" && value != "no" && value != "off" && value != "0"
	}
	if depth := root.child("fetch-depth"); depth != nil {
		if value, err := strconv.ParseInt(qtTrim(depth.scalar), 10, 32); err == nil && value >= 0 {
			wf.FetchDepth = int(value)
		}
	}
	var jobIDs []string
	jobs := root.child("jobs")
	if jobs != nil && jobs.kind == nodeMap {
		for _, key := range jobs.sortedKeys() {
			jobIDs = append(jobIDs, qtTrim(key))
		}
	}
	addNeeds := func(n *node) {
		for _, raw := range scalarOrList(n) {
			token := qtTrim(raw)
			if token == "" {
				continue
			}
			sameFileJob := false
			for _, job := range jobIDs {
				if equalFoldQt(job, token) {
					sameFileJob = true
				}
			}
			if sameFileJob || containsString(wf.Needs, token) {
				continue
			}
			wf.Needs = append(wf.Needs, token)
		}
	}
	addNeeds(root.child("needs"))
	if env := root.child("env"); env != nil && env.kind == nodeMap {
		for key, value := range env.keys {
			wf.Env[key] = value.scalar
		}
	}
	wf.Steps = collectSteps(root.child("steps"), wf.Steps)
	if jobs != nil && jobs.kind == nodeMap {
		for _, key := range jobs.sortedKeys() {
			job := jobs.keys[key]
			addRunsOn(job.child("runs-on"))
			addNeeds(job.child("needs"))
			wf.Steps = collectSteps(job.child("steps"), wf.Steps)
		}
	}
	if len(wf.Steps) == 0 {
		wf.Error = "workflow has no runnable steps"
		return wf
	}
	wf.Valid = true
	return wf
}

func (w Workflow) MatchesPushChangedPaths(changed []string) bool {
	if len(w.PushChangedPaths) == 0 {
		return true
	}
	if len(changed) == 0 {
		return false
	}
	for _, changedPath := range changed {
		for _, pattern := range w.PushChangedPaths {
			normalized := qtTrim(pattern)
			if normalized == "" {
				continue
			}
			if re, ok := wildcardRegexp(normalized); ok && re.MatchString(changedPath) {
				return true
			}
		}
	}
	return false
}

const anyNodeLabel = "any"

func RunsOnNode(runsOn, nodeLabels []string) bool {
	if len(runsOn) == 0 {
		return true
	}
	for _, wanted := range runsOn {
		if wanted == anyNodeLabel {
			return true
		}
		for _, have := range nodeLabels {
			if equalFoldQt(wanted, qtTrim(have)) {
				return true
			}
		}
	}
	return false
}

var labelSeparators = regexp.MustCompile(`[,;\t\n\v\f\r ]+`)

func ParseLabelList(configured string) []string {
	var out []string
	for _, part := range labelSeparators.Split(configured, -1) {
		if part == "" {
			continue
		}
		label := strings.ToLower(qtTrim(part))
		if label != "" && !containsString(out, label) {
			out = append(out, label)
		}
	}
	return out
}

func PinnedNode(pins []string, workflowPath string) string {
	if workflowPath == "" {
		return ""
	}
	for _, entry := range pins {
		sep := strings.IndexByte(entry, '\t')
		if sep <= 0 {
			continue
		}
		if entry[:sep] == workflowPath {
			return strings.ToLower(qtTrim(entry[sep+1:]))
		}
	}
	return ""
}

func SetPinnedNode(pins, paths []string, nodeName string) []string {
	byPath := map[string]string{}
	for _, entry := range pins {
		if sep := strings.IndexByte(entry, '\t'); sep > 0 {
			byPath[entry[:sep]] = strings.ToLower(qtTrim(entry[sep+1:]))
		}
	}
	label := strings.ToLower(qtTrim(nodeName))
	for _, p := range paths {
		if p == "" {
			continue
		}
		if label == "" {
			delete(byPath, p)
		} else {
			byPath[p] = label
		}
	}
	keys := make([]string, 0, len(byPath))
	for key := range byPath {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool { return qtKeyLess(keys[i], keys[j]) })
	out := make([]string, 0, len(keys))
	for _, key := range keys {
		out = append(out, key+"\t"+byPath[key])
	}
	return out
}

func osLabel() string {
	switch runtime.GOOS {
	case "darwin":
		return "macos"
	case "windows":
		return "windows"
	case "linux":
		return "linux"
	}
	return ""
}

func NodeLabels(machineNode, mirrorNode, configured string) []string {
	var out []string
	add := func(raw string) {
		label := strings.ToLower(qtTrim(raw))
		if label != "" && !containsString(out, label) {
			out = append(out, label)
		}
	}
	add(machineNode)
	add(mirrorNode)
	add(osLabel())
	for _, label := range ParseLabelList(configured) {
		add(label)
	}
	return out
}

func WorkflowRunsOnLabels(pins []string, wf Workflow) []string {
	if pin := PinnedNode(pins, wf.Path); pin != "" {
		return []string{pin}
	}
	return wf.RunsOn
}

func WorkflowRunsOnThisNode(pins []string, wf Workflow, nodeLabels []string) bool {
	return RunsOnNode(WorkflowRunsOnLabels(pins, wf), nodeLabels)
}

var (
	varsReference  = regexp.MustCompile(`\$\{\{[\t\n\v\f\r ]*vars\.([A-Za-z_][A-Za-z0-9_]*)[\t\n\v\f\r ]*\}\}`)
	braceReference = regexp.MustCompile(`\$\{([A-Za-z_][A-Za-z0-9_]*)\}`)
	plainReference = regexp.MustCompile(`\$([A-Za-z_][A-Za-z0-9_]*)`)
)

func Substitute(input string, vars map[string]string) string {
	result := varsReference.ReplaceAllStringFunc(input, func(match string) string {
		name := varsReference.FindStringSubmatch(match)[1]
		return vars[name]
	})
	return braceReference.ReplaceAllStringFunc(result, func(match string) string {
		name := braceReference.FindStringSubmatch(match)[1]
		if value, ok := vars[name]; ok {
			return value
		}
		return match
	})
}

func DefaultBranchRetiresPush(relPath, defaultBranchContent string) bool {
	if qtTrim(defaultBranchContent) == "" {
		return false
	}
	onDefault := Parse(relPath, defaultBranchContent)
	return onDefault.Valid && !onDefault.TriggersOnPush()
}

func ParseDir(checkoutDir string) []Workflow {
	var out []Workflow
	dir := filepath.Join(checkoutDir, ".forkmesh")
	entries, err := os.ReadDir(dir)
	if err != nil {
		return out
	}
	var names []string
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		lower := strings.ToLower(name)
		if !strings.HasSuffix(lower, ".yml") && !strings.HasSuffix(lower, ".yaml") {
			continue
		}
		info, err := os.Stat(filepath.Join(dir, name))
		if err != nil || !info.Mode().IsRegular() {
			continue
		}
		names = append(names, name)
	}
	sort.Slice(names, func(i, j int) bool { return qtKeyLess(names[i], names[j]) })
	for _, name := range names {
		data, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			continue
		}
		out = append(out, Parse(path.Join(".forkmesh", name), qtFromUtf8(stripCarriageReturns(data))))
	}
	return out
}
