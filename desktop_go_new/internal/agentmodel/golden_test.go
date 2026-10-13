package agentmodel

import (
	"encoding/json"
	"os"
	"sort"
	"testing"

	"forkmesh.com/desktop-core/internal/qtjson"
)

func qtValue(value any) qtjson.Value {
	switch v := value.(type) {
	case qtjson.Value:
		return v
	case string:
		return qtjson.NewString(v)
	case bool:
		return qtjson.NewBool(v)
	}
	return qtjson.From(value)
}

func windowsOf(value qtjson.Value) []usageWindow {
	out := []usageWindow{}
	for _, row := range value.Items() {
		out = append(out, usageWindow{ID: row.Get("id").Str(), Label: row.Get("label").Str(),
			UsedPercent: row.Get("usedPercent").IntOr(-1), ResetAtMs: row.Get("resetAtMs").Int64(),
			Exhausted: row.Get("exhausted").BoolValue()})
	}
	return out
}

func doubleOr(value qtjson.Value, fallback float64) float64 {
	if value.IsNumber() {
		return value.Float()
	}
	return fallback
}

func goldenRun(t *testing.T, fn string, a qtjson.Value) qtjson.Value {
	s := func(key string) string { return a.Get(key).Str() }
	i := func(key string) int { return a.Get(key).Int() }
	b := func(key string) bool { return a.Get(key).BoolValue() }
	switch fn {
	case "approvalSummary":
		return qtValue(approvalSummary(b("allow"), s("lifetime"), s("question"), s("title")))
	case "mergeSummary":
		return qtValue(mergeSummary(s("branch"), s("base"), s("repo")))
	case "deleteSummary":
		return qtValue(deleteSummary(s("branch"), s("repo"), b("running")))
	case "outsideDeleteSummary":
		return qtValue(outsideDeleteSummary(s("label"), s("cwd")))
	case "bulkSummary":
		return qtValue(bulkSummary(s("action"), i("count")))
	case "purgeSummary":
		return qtValue(purgeSummary(i("count")))
	case "revokeSummary":
		return qtValue(revokeSummary(s("capability"), s("resource")))
	case "numstatRows":
		return numstatRows(s("text"), maxDiffFiles)
	case "capDiff":
		truncated := false
		text := capDiff(toU16(s("text")), i("budget"), &truncated)
		return qtjson.ObjectOf("text", fromU16(text), "truncated", truncated)
	case "untrackedPaths":
		return qtjson.NewStrings(untrackedPaths(s("text"), maxUntracked))
	case "diffImageMime":
		return qtValue(diffImageMime(s("path")))
	case "validDiffImagePath":
		return qtValue(validDiffImagePath(s("path")))
	case "worktrees":
		rows := qtjson.NewArray()
		sessions := a.Get("sessions")
		for index, entry := range parseWorktreePorcelain(s("text"), maxWorktrees) {
			if entry.Bare {
				continue
			}
			rows.Append(worktreeRow(entry, index == 0, i("ahead"), i("behind"), sessions.Get(entry.Branch).Int()))
		}
		return rows
	case "parseLeftRight":
		behind, ahead, ok := parseLeftRight(s("text"))
		return qtjson.ObjectOf("ok", ok, "behind", behind, "ahead", ahead)
	case "mergeShortstat":
		out := qtjson.NewArray()
		for _, merge := range parseMergeShortstatLog(s("log")) {
			out.Append(qtjson.ObjectOf("sha", merge.SHA, "subject", merge.Subject, "files", merge.Files,
				"insertions", merge.Insertions, "deletions", merge.Deletions))
		}
		return out
	case "mergedBranch":
		return qtValue(mergedBranchFromMergeSubject(s("subject")))
	case "mergeStats":
		timestamps := []int64{}
		for _, item := range a.Get("timestamps").Items() {
			timestamps = append(timestamps, item.Int64())
		}
		stats := mergeStats{Base: s("base"), Commit: s("commit"), Commits: a.Get("commits").IntOr(-1),
			Files: i("files"), Insertions: i("insertions"), Deletions: i("deletions"),
			Velocity: velocityWindows(mergeVelocityForTimestamps(timestamps, a.Get("now").Int64()))}
		return mergeStatsJSON(stats)
	case "binTitle":
		return qtValue(binTitle(i("issueNumber"), s("issueTitle"), s("prompt")))
	case "binPayload":
		entries := []binEntry{}
		for _, row := range a.Get("entries").Items() {
			entries = append(entries, binEntry{ID: row.Get("id").Int(), Title: row.Get("title").Str(),
				Owner: row.Get("owner").Str(), Name: row.Get("name").Str(), Branch: row.Get("branch").Str(),
				Provider: row.Get("provider").Str(), DeletedAtMs: row.Get("deletedAtMs").Int64()})
		}
		return binPayload(entries, i("retentionHours"), i("minHours"), i("maxHours"), a.Get("now").Int64())
	case "usage":
		providers := []usageProvider{}
		for _, row := range a.Get("providers").Items() {
			provider := usageProvider{ID: row.Get("id").Str(), Label: row.Get("label").Str(),
				Windows: windowsOf(row.Get("windows")), CreditsEnabled: row.Get("creditsEnabled").BoolValue(),
				CreditsUsed: doubleOr(row.Get("creditsUsed"), -1), CreditsLimit: doubleOr(row.Get("creditsLimit"), -1),
				CreditsCurr: row.Get("creditsCurrency").Str()}
			for _, acc := range row.Get("accounts").Items() {
				provider.Accounts = append(provider.Accounts, usageAccount{ID: acc.Get("id").Str(),
					Label: acc.Get("label").Str(), Active: acc.Get("active").BoolValue(),
					SignedIn: acc.Get("signedIn").BoolValue(), Identity: acc.Get("identity").Str(),
					Windows: windowsOf(acc.Get("windows"))})
			}
			providers = append(providers, provider)
		}
		summary := usageSummary(providers, doubleOr(a.Get("claudeSpendUsd"), -1), doubleOr(a.Get("openAiSpendUsd"), -1),
			a.Get("now").Int64())
		summary.Set("activeProvider", qtjson.NewString(s("activeProvider")))
		return summary
	case "grantRows":
		return grantRows(a.Get("grants"), maxGrants)
	case "canonicalIds":
		ids, ok := canonicalIDs(a.Get("ids"), a.Get("max").IntOr(maxBulkIDs))
		list := qtjson.NewArray()
		if ok {
			list = intsJSON(ids)
		}
		return qtjson.ObjectOf("ok", ok, "ids", list)
	case "bulkTarget":
		ids := []int{}
		for _, item := range a.Get("ids").Items() {
			ids = append(ids, item.Int())
		}
		return qtValue(bulkTarget(s("action"), ids))
	case "answerTarget":
		return qtValue(answerTarget(s("id"), b("allow"), s("lifetime")))
	case "validGrantId":
		return qtValue(validGrantID(s("id")))
	case "validApprovalId":
		return qtValue(validApprovalID(s("id")))
	case "outsideEntry":
		return qtValue(outsideEntry(s("kind"), a.Get("entry")))
	}
	t.Fatalf("unknown golden function %s", fn)
	return qtjson.Value{}
}

func TestHelpersMatchCppGolden(t *testing.T) {
	casesData, err := os.ReadFile("testdata/ops_cases.json")
	if err != nil {
		t.Fatal(err)
	}
	goldenData, err := os.ReadFile("testdata/ops_golden.json")
	if err != nil {
		t.Fatal(err)
	}
	cases, err := qtjson.Parse(casesData)
	if err != nil {
		t.Fatal(err)
	}
	var golden []string
	if err := json.Unmarshal(goldenData, &golden); err != nil {
		t.Fatal(err)
	}
	if cases.Len() != len(golden) {
		t.Fatalf("%d cases, %d golden answers", cases.Len(), len(golden))
	}
	failures := map[string]int{}
	for index, item := range cases.Items() {
		fn := item.Get("fn").Str()
		got := string(goldenRun(t, fn, item.Get("args")).Compact())
		if got != golden[index] {
			failures[fn]++
			if failures[fn] <= 2 {
				t.Errorf("case %d %s\n got %s\nwant %s", index, fn, got, golden[index])
			}
		}
	}
	names := []string{}
	for name := range failures {
		names = append(names, name)
	}
	sort.Strings(names)
	if len(names) > 0 {
		t.Fatalf("failing functions: %v", names)
	}
}
