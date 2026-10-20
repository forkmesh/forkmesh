package mirrornode

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const (
	maxIssueRecordBytes = 2 << 20
	maxIssueGitOutput   = 256 << 20
)

var issueDirEntryPattern = regexp.MustCompile(
	`^\.forkmesh/issues/(open|closed)/([^/]+)/(.+)$`)

type issueTreeEntry struct {
	mode string
	oid  string
	path string
}

type issueCopy struct {
	side        int
	status      string
	name        string
	number      int
	recordMode  string
	recordOID   string
	record      map[string]any
	files       []issueTreeEntry
	opaque      bool
	placeholder bool
	identity    string
	createdAt   int64
	claim       int
}

type issueEntity struct {
	key         string
	copies      []*issueCopy
	folded      []*issueCopy
	opaque      bool
	placeholder bool
	deleted     bool
	merged      bool
	claim       int
	floor       int
	createdAt   int64
	number      int
}

type issueOutput struct {
	dir         string
	record      []byte
	recordEntry issueTreeEntry
	files       []issueTreeEntry
}

func (c *issueCopy) dir() string {
	return ".forkmesh/issues/" + c.status + "/" + c.name + "/"
}

func issueDirFor(status string, number int) string {
	return ".forkmesh/issues/" + status + "/" + strconv.Itoa(number) + "/"
}

func reconcileIssueOnlyDivergence(
	ctx context.Context, gitDir, left, right string,
) (string, bool, error) {
	baseRaw, err := issueGitOutput(ctx, maxIssueGitOutput, nil,
		"--git-dir="+gitDir, "merge-base", "--all", left, right)
	if err != nil {
		return "", false, nil
	}
	bases := strings.Fields(string(baseRaw))
	if len(bases) == 0 {
		return "", false, nil
	}
	for _, candidate := range bases {
		if !gitObjectIDPattern.MatchString(candidate) {
			return "", false, nil
		}
	}
	if gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", left+":.forkmesh/issues") ==
		gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", right+":.forkmesh/issues") {
		return "", false, nil
	}

	seedTree, err := issueMergeSeedTree(ctx, gitDir, left, right)
	if err != nil {
		return "", true, err
	}
	var copies, baseCopies []*issueCopy
	for side, commit := range []string{left, right} {
		found, err := issueCopiesAt(ctx, gitDir, commit, side)
		if err != nil {
			return "", true, err
		}
		copies = append(copies, found...)
	}
	for _, base := range bases {
		found, err := issueCopiesAt(ctx, gitDir, base, -1)
		if err != nil {
			return "", true, err
		}
		baseCopies = append(baseCopies, found...)
	}
	if err := loadIssueRecords(ctx, gitDir, append(append([]*issueCopy{}, copies...), baseCopies...)); err != nil {
		return "", true, err
	}
	outputs := planIssueMerge(copies, baseCopies)

	final := map[string]issueTreeEntry{}
	put := func(entry issueTreeEntry) {
		if existing, ok := final[entry.path]; ok && existing.oid <= entry.oid {
			return
		}
		final[entry.path] = entry
	}
	for _, output := range outputs {
		record := output.recordEntry
		if output.record != nil {
			oidRaw, err := issueGitInput(ctx, 256, output.record, nil,
				"--git-dir="+gitDir, "hash-object", "-w", "--stdin")
			if err != nil {
				return "", true, fmt.Errorf("write merged issue record: %w", err)
			}
			record.oid = strings.TrimSpace(string(oidRaw))
			if !gitObjectIDPattern.MatchString(record.oid) {
				return "", true, errors.New("merged issue blob id is invalid")
			}
		}
		if record.oid != "" {
			record.path = output.dir + record.path
			put(record)
		}
		for _, file := range output.files {
			file.path = output.dir + file.path
			put(file)
		}
	}

	index, err := os.CreateTemp("", "forkmesh-issue-merge-index-*")
	if err != nil {
		return "", true, err
	}
	indexPath := index.Name()
	if closeErr := index.Close(); closeErr != nil {
		_ = os.Remove(indexPath)
		return "", true, closeErr
	}
	_ = os.Remove(indexPath)
	defer os.Remove(indexPath)
	indexEnv := append(os.Environ(), "GIT_INDEX_FILE="+indexPath)
	if _, err := issueGitOutput(ctx, maxIssueGitOutput, indexEnv,
		"--git-dir="+gitDir, "read-tree", seedTree); err != nil {
		return "", true, fmt.Errorf("seed issue merge tree: %w", err)
	}
	seedRaw, err := issueGitOutput(ctx, maxIssueGitOutput, nil,
		"--git-dir="+gitDir, "ls-tree", "-r", "-z", "--name-only", seedTree,
		"--", ".forkmesh/issues/open", ".forkmesh/issues/closed")
	if err != nil {
		return "", true, fmt.Errorf("read seeded issue paths: %w", err)
	}
	zero := strings.Repeat("0", len(seedTree))
	var info bytes.Buffer
	for _, pathRaw := range bytes.Split(seedRaw, []byte{0}) {
		path := string(pathRaw)
		if !issueDirEntryPattern.MatchString(path) {
			continue
		}
		if _, ok := final[path]; !ok {
			fmt.Fprintf(&info, "0 %s\t%s\x00", zero, path)
		}
	}
	paths := make([]string, 0, len(final))
	for path := range final {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	for _, path := range paths {
		entry := final[path]
		fmt.Fprintf(&info, "%s %s\t%s\x00", entry.mode, entry.oid, path)
	}
	if info.Len() > 0 {
		if _, err := issueGitInput(ctx, maxIssueGitOutput, info.Bytes(), indexEnv,
			"--git-dir="+gitDir, "update-index", "-z", "--index-info"); err != nil {
			return "", true, fmt.Errorf("stage merged issues: %w", err)
		}
	}
	treeRaw, err := issueGitOutput(ctx, 256, indexEnv,
		"--git-dir="+gitDir, "write-tree")
	if err != nil {
		return "", true, fmt.Errorf("write issue merge tree: %w", err)
	}
	tree := strings.TrimSpace(string(treeRaw))
	if !gitObjectIDPattern.MatchString(tree) {
		return "", true, errors.New("issue merge tree id is invalid")
	}
	commit, err := writeReconcilerMerge(ctx, gitDir, tree, left, right,
		"Merge divergent mirror issue intake\n")
	if err != nil {
		return "", true, err
	}
	return commit, true, nil
}

func writeReconcilerMerge(ctx context.Context, gitDir, tree, left, right, message string) (string, error) {
	parents := []string{left, right}
	sort.Strings(parents)
	commitAt := gitCommitTime(ctx, gitDir, parents[0])
	if other := gitCommitTime(ctx, gitDir, parents[1]); other > commitAt {
		commitAt = other
	}
	commitAt++
	date := fmt.Sprintf("@%d +0000", commitAt)
	commitEnv := append(os.Environ(),
		"GIT_AUTHOR_NAME="+commitIdentityName("ForkMesh Mirror Reconciler"),
		"GIT_AUTHOR_EMAIL="+commitIdentityEmail("mirror-reconciler"),
		"GIT_COMMITTER_NAME="+commitIdentityName("ForkMesh Mirror Reconciler"),
		"GIT_COMMITTER_EMAIL="+commitIdentityEmail("mirror-reconciler"),
		"GIT_AUTHOR_DATE="+date,
		"GIT_COMMITTER_DATE="+date)
	commitRaw, err := issueGitInput(ctx, 256, []byte(message), commitEnv,
		"--git-dir="+gitDir, "commit-tree", tree,
		"-p", parents[0], "-p", parents[1])
	if err != nil {
		return "", fmt.Errorf("write reconciler merge commit: %w", err)
	}
	commit := strings.TrimSpace(string(commitRaw))
	if !gitObjectIDPattern.MatchString(commit) {
		return "", errors.New("reconciler merge commit id is invalid")
	}
	return commit, nil
}

func issueMergeSeedTree(ctx context.Context, gitDir, left, right string) (string, error) {
	command := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "merge-tree",
		"--write-tree", "--name-only", "--no-messages", "-z", left, right)
	var stdout, stderr bytes.Buffer
	command.Stdout = &stdout
	command.Stderr = &stderr
	runErr := command.Run()
	var exitErr *exec.ExitError
	if runErr != nil && !(errors.As(runErr, &exitErr) && exitErr.ExitCode() == 1) {
		return "", fmt.Errorf("merge divergent trees: %s", boundedText(stderr.Bytes(), 200))
	}
	parts := strings.Split(stdout.String(), "\x00")
	tree := parts[0]
	if newline := strings.IndexByte(tree, '\n'); newline >= 0 {
		parts = append([]string{tree[:newline], tree[newline+1:]}, parts[1:]...)
		tree = parts[0]
	}
	tree = strings.TrimSpace(tree)
	if !gitObjectIDPattern.MatchString(tree) {
		return "", errors.New("merged tree id is invalid")
	}
	if runErr == nil {
		return tree, nil
	}
	for _, path := range parts[1:] {
		if path == "" || strings.HasPrefix(path, ".forkmesh/issues/open/") ||
			strings.HasPrefix(path, ".forkmesh/issues/closed/") {
			continue
		}
		newer := left
		leftAt, rightAt := gitCommitTime(ctx, gitDir, left), gitCommitTime(ctx, gitDir, right)
		if rightAt > leftAt || (rightAt == leftAt && right > left) {
			newer = right
		}
		tree = gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", newer+"^{tree}")
		if !gitObjectIDPattern.MatchString(tree) {
			return "", errors.New("newer side tree id is invalid")
		}
		return tree, nil
	}
	return tree, nil
}

func issueCopiesAt(ctx context.Context, gitDir, commit string, side int) ([]*issueCopy, error) {
	raw, err := issueGitOutput(ctx, maxIssueGitOutput, nil,
		"--git-dir="+gitDir, "ls-tree", "-r", "-z", commit,
		"--", ".forkmesh/issues/open", ".forkmesh/issues/closed")
	if err != nil {
		return nil, fmt.Errorf("read issue tree: %w", err)
	}
	groups := map[string]*issueCopy{}
	var order []*issueCopy
	for _, item := range bytes.Split(raw, []byte{0}) {
		headerPath := bytes.SplitN(item, []byte{'\t'}, 2)
		if len(headerPath) != 2 {
			continue
		}
		fields := strings.Fields(string(headerPath[0]))
		path := string(headerPath[1])
		match := issueDirEntryPattern.FindStringSubmatch(path)
		if len(fields) != 3 || match == nil {
			continue
		}
		key := match[1] + "/" + match[2]
		copy := groups[key]
		if copy == nil {
			copy = &issueCopy{side: side, status: match[1], name: match[2]}
			if number, err := strconv.Atoi(match[2]); err == nil && number > 0 &&
				strconv.Itoa(number) == match[2] {
				copy.number = number
			}
			groups[key] = copy
			order = append(order, copy)
		}
		if copy.number > 0 && fields[1] == "blob" &&
			match[3] == fmt.Sprintf("issue-%d.json", copy.number) {
			copy.recordMode, copy.recordOID = fields[0], fields[2]
			continue
		}
		copy.files = append(copy.files, issueTreeEntry{
			mode: fields[0], oid: fields[2], path: match[3],
		})
	}
	return order, nil
}

func loadIssueRecords(ctx context.Context, gitDir string, copies []*issueCopy) error {
	seen := map[string]bool{}
	var wanted []string
	for _, copy := range copies {
		if copy.recordOID != "" && !seen[copy.recordOID] {
			seen[copy.recordOID] = true
			wanted = append(wanted, copy.recordOID)
		}
	}
	blobs := map[string][]byte{}
	if len(wanted) > 0 {
		raw, err := issueGitInput(ctx, maxIssueGitOutput,
			[]byte(strings.Join(wanted, "\n")+"\n"), nil,
			"--git-dir="+gitDir, "cat-file", "--batch")
		if err != nil {
			return fmt.Errorf("read issue records: %w", err)
		}
		for len(raw) > 0 {
			newline := bytes.IndexByte(raw, '\n')
			if newline < 0 {
				break
			}
			header := strings.Fields(string(raw[:newline]))
			raw = raw[newline+1:]
			if len(header) != 3 {
				continue
			}
			size, err := strconv.Atoi(header[2])
			if err != nil || size < 0 || size > len(raw) {
				return errors.New("issue record batch is malformed")
			}
			blobs[header[0]] = raw[:size]
			raw = raw[size:]
			if len(raw) > 0 && raw[0] == '\n' {
				raw = raw[1:]
			}
		}
	}
	for _, copy := range copies {
		classifyIssueCopy(copy, blobs[copy.recordOID])
	}
	return nil
}

func decodeIssueRecord(raw []byte) (map[string]any, bool) {
	if len(raw) == 0 || len(raw) > maxIssueRecordBytes {
		return nil, false
	}
	var record map[string]any
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	if decoder.Decode(&record) != nil || decoder.Decode(&struct{}{}) != io.EOF || record == nil {
		return nil, false
	}
	return record, true
}

func classifyIssueCopy(copy *issueCopy, raw []byte) {
	record, ok := decodeIssueRecord(raw)
	valid := ok && copy.recordOID != "" &&
		stringValue(record["schema"]) == "forkmesh-issue-v1" &&
		issueNumber(record["number"]) == copy.number
	identity, createdAt := "", int64(0)
	placeholder := valid && issueOpeningIsPlaceholder(record)
	if valid && !placeholder {
		var err error
		identity, createdAt, err = issueMergeIdentity(record)
		valid = err == nil
	}
	if !valid {
		copy.opaque = true
		if copy.recordOID != "" {
			copy.files = append(copy.files, issueTreeEntry{
				mode: copy.recordMode, oid: copy.recordOID,
				path: fmt.Sprintf("issue-%d.json", copy.number),
			})
			sort.Slice(copy.files, func(i, j int) bool {
				return copy.files[i].path < copy.files[j].path
			})
			copy.recordOID, copy.recordMode = "", ""
		}
		return
	}
	copy.record = record
	copy.placeholder = placeholder
	copy.identity = identity
	copy.createdAt = createdAt
	if events, _ := record["events"].([]any); len(events) > 0 {
		if opening, _ := events[0].(map[string]any); opening != nil {
			if at := issueInt64(opening["ts"]); at > 0 {
				copy.createdAt = at
			}
		}
	}
	copy.claim = copy.number
	if intake := issueNumber(record["intakeNumber"]); intake > 0 && intake < copy.number {
		copy.claim = intake
	}
}

func issueCopyKey(copy *issueCopy) string {
	switch {
	case copy.opaque:
		return "opaque:" + copy.status + "/" + copy.name
	case copy.placeholder:
		return "placeholder:" + strconv.Itoa(copy.number)
	}
	return copy.identity
}

func issueEntityBefore(a, b *issueEntity) bool {
	if a.createdAt != b.createdAt {
		return a.createdAt < b.createdAt
	}
	return a.key < b.key
}

func planIssueMerge(copies, baseCopies []*issueCopy) []issueOutput {
	entities := map[string]*issueEntity{}
	var keys []string
	for _, copy := range copies {
		key := issueCopyKey(copy)
		entity := entities[key]
		if entity == nil {
			entity = &issueEntity{key: key, opaque: copy.opaque, placeholder: copy.placeholder}
			entities[key] = entity
			keys = append(keys, key)
		}
		entity.copies = append(entity.copies, copy)
	}
	sort.Strings(keys)
	base := map[string][]*issueCopy{}
	for _, copy := range baseCopies {
		key := issueCopyKey(copy)
		base[key] = append(base[key], copy)
	}
	used := map[int]bool{}
	var reals, placeholders []*issueEntity
	for _, key := range keys {
		entity := entities[key]
		sides := [2]bool{}
		entity.claim, entity.floor, entity.createdAt = 0, 0, 0
		for i, copy := range entity.copies {
			if copy.side >= 0 && copy.side < 2 {
				sides[copy.side] = true
			}
			if i == 0 || copy.claim < entity.claim {
				entity.claim = copy.claim
			}
			if copy.number > entity.floor {
				entity.floor = copy.number
			}
			if i == 0 || copy.createdAt < entity.createdAt {
				entity.createdAt = copy.createdAt
			}
		}
		if _, inBase := base[key]; inBase && !entity.opaque {
			entity.deleted = !sides[0] || !sides[1]
		}
		switch {
		case entity.opaque:
			entity.number = entity.copies[0].number
			if entity.number > 0 {
				used[entity.number] = true
			}
		case entity.placeholder:
			placeholders = append(placeholders, entity)
		case !entity.deleted:
			reals = append(reals, entity)
		}
	}
	winners := map[int]*issueEntity{}
	for _, entity := range reals {
		if entity.floor != entity.claim || used[entity.claim] {
			continue
		}
		if current := winners[entity.claim]; current == nil || issueEntityBefore(entity, current) {
			winners[entity.claim] = entity
		}
	}
	for number, winner := range winners {
		used[number] = true
		winner.number = number
	}
	for _, placeholder := range placeholders {
		if winner := winners[placeholder.claim]; winner != nil {
			winner.folded = append(winner.folded, placeholder.copies...)
			placeholder.merged = true
			continue
		}
		if placeholder.deleted {
			continue
		}
		number := placeholder.floor
		for used[number] {
			number++
		}
		used[number] = true
		placeholder.number = number
	}
	var losers []*issueEntity
	for _, entity := range reals {
		if entity.number == 0 {
			losers = append(losers, entity)
		}
	}
	sort.Slice(losers, func(i, j int) bool {
		if losers[i].claim != losers[j].claim {
			return losers[i].claim < losers[j].claim
		}
		return issueEntityBefore(losers[i], losers[j])
	})
	for _, loser := range losers {
		number := loser.claim + 1
		if loser.floor > number {
			number = loser.floor
		}
		for used[number] {
			number++
		}
		used[number] = true
		loser.number = number
	}
	var outputs []issueOutput
	for _, key := range keys {
		entity := entities[key]
		if entity.merged || entity.deleted || (!entity.opaque && entity.number <= 0) {
			continue
		}
		outputs = append(outputs, renderIssueEntity(entity, base[key]))
	}
	return outputs
}

func issueCopiesIdentical(copies []*issueCopy) bool {
	first := copies[0]
	for _, copy := range copies[1:] {
		if copy.status != first.status || copy.name != first.name ||
			copy.recordOID != first.recordOID || copy.recordMode != first.recordMode ||
			len(copy.files) != len(first.files) {
			return false
		}
		for i := range copy.files {
			if copy.files[i] != first.files[i] {
				return false
			}
		}
	}
	return true
}

func renderIssueEntity(entity *issueEntity, base []*issueCopy) issueOutput {
	first := entity.copies[0]
	if entity.opaque {
		return issueOutput{dir: first.dir(), files: unionIssueFiles(entity.copies)}
	}
	recordName := fmt.Sprintf("issue-%d.json", entity.number)
	if len(entity.folded) == 0 && issueCopiesIdentical(entity.copies) {
		if first.number == entity.number {
			return issueOutput{
				dir: first.dir(), files: first.files,
				recordEntry: issueTreeEntry{mode: first.recordMode, oid: first.recordOID, path: recordName},
			}
		}
		record := map[string]any{}
		for key, value := range first.record {
			record[key] = value
		}
		setIssueNumber(record, entity.number, entity.claim)
		return issueOutput{
			dir: issueDirFor(first.status, entity.number), files: first.files,
			record: marshalIssueRecord(record), recordEntry: issueTreeEntry{mode: "100644", path: recordName},
		}
	}
	record := canonicalIssueRecord(entity.copies, entity.folded, base)
	setIssueNumber(record, entity.number, entity.claim)
	status := "open"
	if stringValue(record["status"]) == "closed" {
		status = "closed"
	}
	all := append(append([]*issueCopy{}, entity.copies...), entity.folded...)
	return issueOutput{
		dir: issueDirFor(status, entity.number), files: unionIssueFiles(all),
		record: marshalIssueRecord(record), recordEntry: issueTreeEntry{mode: "100644", path: recordName},
	}
}

func setIssueNumber(record map[string]any, number, claim int) {
	record["number"] = number
	if claim > 0 && claim < number {
		record["intakeNumber"] = claim
	} else {
		delete(record, "intakeNumber")
	}
}

func marshalIssueRecord(record map[string]any) []byte {
	raw, err := json.MarshalIndent(record, "", "    ")
	if err != nil {
		raw = []byte("{}")
	}
	return append(raw, '\n')
}

func issueJSON(value any) string {
	raw, err := json.Marshal(value)
	if err != nil {
		return fmt.Sprintf("%#v", value)
	}
	return string(raw)
}

func unionIssueFiles(copies []*issueCopy) []issueTreeEntry {
	variants := map[string]map[string]issueTreeEntry{}
	for _, copy := range copies {
		for _, file := range copy.files {
			byOID := variants[file.path]
			if byOID == nil {
				byOID = map[string]issueTreeEntry{}
				variants[file.path] = byOID
			}
			if existing, ok := byOID[file.oid]; !ok || file.mode < existing.mode {
				byOID[file.oid] = file
			}
		}
	}
	chosen := map[string]issueTreeEntry{}
	for path, byOID := range variants {
		oids := make([]string, 0, len(byOID))
		for oid := range byOID {
			oids = append(oids, oid)
		}
		sort.Strings(oids)
		for i, oid := range oids {
			entry := byOID[oid]
			if i > 0 {
				entry.path = path + "~" + oid[:min(12, len(oid))]
			}
			if existing, ok := chosen[entry.path]; !ok || entry.oid < existing.oid {
				chosen[entry.path] = entry
			}
		}
	}
	files := make([]issueTreeEntry, 0, len(chosen))
	for _, entry := range chosen {
		files = append(files, entry)
	}
	sort.Slice(files, func(i, j int) bool { return files[i].path < files[j].path })
	return files
}

func issueEventIdentity(event map[string]any) string {
	id, signature := stringValue(event["id"]), stringValue(event["sig"])
	if id != "" || signature != "" {
		return "id:" + id + "\x00sig:" + signature
	}
	sum := sha256.Sum256([]byte(issueJSON(event)))
	return "content:" + hex.EncodeToString(sum[:])
}

func issueEventIsPlaceholderOpening(event map[string]any) bool {
	if stringValue(event["type"]) != "open" {
		return false
	}
	signature := stringValue(event["sig"])
	return signature == "relay-placeholder" ||
		(signature == "" && strings.TrimSpace(stringValue(event["title"])) == "")
}

func issueRecordEvents(record map[string]any) []map[string]any {
	values, _ := record["events"].([]any)
	events := make([]map[string]any, 0, len(values))
	for _, value := range values {
		if event, _ := value.(map[string]any); event != nil {
			events = append(events, event)
		}
	}
	return events
}

func canonicalIssueRecord(primary, folded, base []*issueCopy) map[string]any {
	var opening map[string]any
	openingKey, openingJSON := "", ""
	for _, copy := range primary {
		events := issueRecordEvents(copy.record)
		if len(events) == 0 {
			continue
		}
		key, encoded := issueEventIdentity(events[0]), issueJSON(events[0])
		if opening == nil || key < openingKey || (key == openingKey && encoded < openingJSON) {
			opening, openingKey, openingJSON = events[0], key, encoded
		}
	}
	collected := map[string]map[string]any{}
	for _, copy := range append(append([]*issueCopy{}, primary...), folded...) {
		for _, event := range issueRecordEvents(copy.record) {
			key := issueEventIdentity(event)
			if key == openingKey || issueEventIsPlaceholderOpening(event) {
				continue
			}
			if existing, ok := collected[key]; ok && issueJSON(existing) <= issueJSON(event) {
				continue
			}
			collected[key] = event
		}
	}
	order := make([]string, 0, len(collected))
	for key := range collected {
		order = append(order, key)
	}
	sort.Slice(order, func(i, j int) bool {
		a, b := issueInt64(collected[order[i]]["ts"]), issueInt64(collected[order[j]]["ts"])
		if a != b {
			return a < b
		}
		return order[i] < order[j]
	})
	rest := make([]map[string]any, 0, len(order))
	for _, key := range order {
		rest = append(rest, collected[key])
	}
	events := make([]any, 0, len(rest)+1)
	if opening != nil {
		events = append(events, opening)
	}
	for _, event := range rest {
		events = append(events, event)
	}

	fields := map[string]bool{}
	for _, copy := range primary {
		for key := range copy.record {
			fields[key] = true
		}
	}
	names := make([]string, 0, len(fields))
	for key := range fields {
		names = append(names, key)
	}
	sort.Strings(names)
	record := map[string]any{}
	for _, key := range names {
		switch key {
		case "events", "number", "intakeNumber", "updatedAt":
			continue
		}
		if value, present := resolveIssueField(key, primary, base, rest); present {
			record[key] = value
		}
	}
	record["events"] = events
	updatedAt := int64(0)
	for _, copy := range append(append([]*issueCopy{}, primary...), folded...) {
		if at := issueInt64(copy.record["updatedAt"]); at > updatedAt {
			updatedAt = at
		}
	}
	for _, event := range rest {
		if at := issueInt64(event["ts"]); at > updatedAt {
			updatedAt = at
		}
	}
	record["updatedAt"] = updatedAt
	return record
}

func issueProjectedKind(field string) string {
	switch field {
	case "title", "status", "labels", "milestone", "priority", "progress":
		return field
	case "startDate", "endDate":
		return "dates"
	}
	return ""
}

func resolveIssueField(
	key string, primary, base []*issueCopy, rest []map[string]any,
) (any, bool) {
	variants := map[string]any{}
	for _, copy := range primary {
		value, ok := copy.record[key]
		encoded := ""
		if ok {
			encoded = issueJSON(value)
		}
		variants[encoded] = value
	}
	switch key {
	case "votes":
		for _, event := range rest {
			if stringValue(event["type"]) == "vote" {
				return issueMergedVotes(primary, rest), true
			}
		}
	case "assignees":
		if value, ok := issueMergedAssignees(primary, rest); ok {
			return value, true
		}
	default:
		if kind := issueProjectedKind(key); kind != "" {
			var latest map[string]any
			for _, event := range rest {
				if stringValue(event["type"]) == kind {
					latest = event
				}
			}
			if latest != nil {
				scratch := map[string]any{}
				applyIssueProjection(scratch, latest)
				value, ok := scratch[key]
				return value, ok
			}
		}
	}
	if len(variants) == 1 {
		for encoded, value := range variants {
			return value, encoded != ""
		}
	}
	inBase := map[string]bool{}
	for _, copy := range base {
		if copy.record == nil {
			continue
		}
		encoded := ""
		if value, ok := copy.record[key]; ok {
			encoded = issueJSON(value)
		}
		inBase[encoded] = true
	}
	var candidates, all []string
	for encoded := range variants {
		all = append(all, encoded)
		if !inBase[encoded] {
			candidates = append(candidates, encoded)
		}
	}
	if len(candidates) == 0 {
		candidates = all
	}
	sort.Strings(candidates)
	chosen := candidates[len(candidates)-1]
	return variants[chosen], chosen != ""
}

func issueMergedVotes(primary []*issueCopy, rest []map[string]any) int64 {
	count := int64(0)
	for _, event := range rest {
		if stringValue(event["type"]) == "vote" {
			count++
		}
	}
	initial := int64(0)
	for _, copy := range primary {
		own := int64(0)
		for _, event := range issueRecordEvents(copy.record) {
			if stringValue(event["type"]) == "vote" {
				own++
			}
		}
		if value := issueInt64(copy.record["votes"]) - own; value > initial {
			initial = value
		}
	}
	return initial + count
}

func issueMergedAssignees(primary []*issueCopy, rest []map[string]any) (any, bool) {
	var latest map[string]any
	var claims []map[string]any
	for _, event := range rest {
		switch stringValue(event["type"]) {
		case "assignees":
			latest, claims = event, nil
		case "claim":
			claims = append(claims, event)
		}
	}
	if latest == nil && len(claims) == 0 {
		return nil, false
	}
	if latest != nil && len(claims) == 0 {
		return latest["assignees"], true
	}
	list := []any{}
	if latest != nil {
		values, _ := latest["assignees"].([]any)
		list = append(list, values...)
	} else {
		mentioned := map[string]bool{}
		for _, claim := range claims {
			mentioned[strings.ToLower(strings.TrimSpace(stringValue(claim["assignee"])))] = true
		}
		ordered := append([]*issueCopy{}, primary...)
		sort.Slice(ordered, func(i, j int) bool { return ordered[i].recordOID < ordered[j].recordOID })
		present := map[string]bool{}
		for _, copy := range ordered {
			values, _ := copy.record["assignees"].([]any)
			for _, value := range values {
				name := strings.ToLower(strings.TrimSpace(stringValue(value)))
				if mentioned[name] || present[name] {
					continue
				}
				present[name] = true
				list = append(list, value)
			}
		}
	}
	for _, claim := range claims {
		list = applyIssueClaim(list, claim)
	}
	return list, true
}

func issueMergeIdentity(record map[string]any) (string, int64, error) {
	events, _ := record["events"].([]any)
	if len(events) == 0 {
		return "", 0, errors.New("record has no opening event")
	}
	opening, _ := events[0].(map[string]any)
	if opening == nil || stringValue(opening["type"]) != "open" {
		return "", 0, errors.New("record has no valid opening event")
	}
	createdAt := issueInt64(record["createdAt"])
	if createdAt <= 0 {
		createdAt = issueInt64(opening["ts"])
	}
	if createdAt <= 0 {
		return "", 0, errors.New("record has no valid creation time")
	}
	if signature := stringValue(opening["sig"]); signature != "" {
		return "sig:" + signature, createdAt, nil
	}
	author := stringValue(opening["author"])
	title := stringValue(opening["title"])
	eventAt := issueInt64(opening["ts"])
	if author == "" || title == "" || eventAt <= 0 {
		return "", 0, errors.New("unsigned opening identity is incomplete")
	}
	return fmt.Sprintf("unsigned:%s:%d:%s", author, eventAt, title), createdAt, nil
}

func issueInt64(value any) int64 {
	switch number := value.(type) {
	case json.Number:
		parsed, _ := strconv.ParseInt(number.String(), 10, 64)
		return parsed
	case float64:
		return int64(number)
	case int64:
		return number
	case int:
		return int64(number)
	}
	return 0
}

func issueGitOutput(
	ctx context.Context, limit int64, env []string, arguments ...string,
) ([]byte, error) {
	return issueGitInput(ctx, limit, nil, env, arguments...)
}

func issueGitInput(
	ctx context.Context, limit int64, input []byte, env []string, arguments ...string,
) ([]byte, error) {
	command := exec.CommandContext(ctx, "git", arguments...)
	if env != nil {
		command.Env = env
	}
	if input != nil {
		command.Stdin = bytes.NewReader(input)
	}
	stdout, err := command.StdoutPipe()
	if err != nil {
		return nil, err
	}
	var stderr bytes.Buffer
	command.Stderr = &stderr
	if err := command.Start(); err != nil {
		return nil, err
	}
	raw, readErr := io.ReadAll(io.LimitReader(stdout, limit+1))
	if int64(len(raw)) > limit {
		_ = command.Process.Kill()
		_ = command.Wait()
		return nil, errors.New("git output exceeded the reconciliation limit")
	}
	waitErr := command.Wait()
	if readErr != nil {
		return nil, readErr
	}
	if waitErr != nil {
		return nil, fmt.Errorf("git %s: %s", arguments[0], boundedText(stderr.Bytes(), 200))
	}
	return raw, nil
}
