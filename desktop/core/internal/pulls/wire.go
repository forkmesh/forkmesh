package pulls

import (
	"math"
	"strconv"
	"strings"
)

func jsonString(obj map[string]any, key string) string {
	if text, ok := obj[key].(string); ok {
		return text
	}
	return ""
}

func jsonStringOr(obj map[string]any, key, fallback string) string {
	if text, ok := obj[key].(string); ok {
		return text
	}
	return fallback
}

func jsonInt(obj map[string]any, key string) int {
	number, ok := obj[key].(float64)
	if !ok || math.IsNaN(number) || math.IsInf(number, 0) || number != math.Trunc(number) ||
		number < math.MinInt32 || number > math.MaxInt32 {
		return 0
	}
	return int(number)
}

func jsonDouble(obj map[string]any, key string) float64 {
	if number, ok := obj[key].(float64); ok {
		return number
	}
	return 0
}

func jsonBool(obj map[string]any, key string) bool {
	value, _ := obj[key].(bool)
	return value
}

func jsonLongLong(obj map[string]any, key string) int64 {
	switch value := obj[key].(type) {
	case float64:
		if math.IsNaN(value) || math.IsInf(value, 0) {
			return 0
		}
		return int64(math.Round(value))
	case string:
		parsed, err := strconv.ParseInt(strings.TrimSpace(value), 10, 64)
		if err != nil {
			return 0
		}
		return parsed
	case bool:
		if value {
			return 1
		}
	}
	return 0
}

func (e *Event) ToJSON() map[string]any {
	return map[string]any{
		"type": e.Type, "id": e.ID, "author": e.Author, "authorName": e.AuthorName,
		"ts": float64(e.Ts), "body": e.Body, "state": e.State, "path": e.Path, "side": e.Side,
		"line": e.Line, "threadId": e.ThreadID, "parentId": e.ParentID, "lineStart": e.LineStart,
		"lineEnd": e.LineEnd, "suggestionPatch": e.SuggestionPatch, "targetPath": e.TargetPath,
		"appliedCommit": e.AppliedCommit, "patchset": e.Patchset, "anchorLines": e.AnchorLines, "sig": e.Sig,
	}
}

func EventFromJSON(obj map[string]any) Event {
	return Event{
		Type:            jsonString(obj, "type"),
		ID:              jsonString(obj, "id"),
		Author:          jsonString(obj, "author"),
		AuthorName:      jsonString(obj, "authorName"),
		Ts:              jsonLongLong(obj, "ts"),
		Body:            jsonString(obj, "body"),
		State:           jsonString(obj, "state"),
		Path:            jsonString(obj, "path"),
		Side:            jsonString(obj, "side"),
		Line:            jsonInt(obj, "line"),
		ThreadID:        jsonString(obj, "threadId"),
		ParentID:        jsonString(obj, "parentId"),
		LineStart:       jsonInt(obj, "lineStart"),
		LineEnd:         jsonInt(obj, "lineEnd"),
		SuggestionPatch: jsonString(obj, "suggestionPatch"),
		TargetPath:      jsonString(obj, "targetPath"),
		AppliedCommit:   jsonString(obj, "appliedCommit"),
		Patchset:        jsonString(obj, "patchset"),
		AnchorLines:     jsonString(obj, "anchorLines"),
		Sig:             jsonString(obj, "sig"),
	}
}

func (p *Pull) WireJSON() map[string]any {
	return map[string]any{
		"number": p.Number, "title": p.Title, "description": p.Description, "base": p.Base, "head": p.Head,
		"status": p.Status, "ts": float64(p.Ts), "author": p.Author, "authorName": p.AuthorName, "sig": p.Sig,
		"patch": p.Patch, "commits": p.Commits, "creationBaseOid": p.CreationBaseOid,
		"creationHeadOid": p.CreationHeadOid, "client": p.Client, "clientSig": p.ClientSig,
		"rewardEstimateLamports": float64(p.RewardEstimateLamports), "rewardRecipient": p.RewardRecipient,
		"rewardId": p.RewardID, "rewardCondition": p.RewardCondition, "rewardStatus": p.RewardStatus,
		"rewardTxSignature": p.RewardTxSignature,
	}
}

func PullFromJSON(obj map[string]any) Pull {
	pr := Pull{
		Number:                 jsonInt(obj, "number"),
		Title:                  jsonString(obj, "title"),
		Base:                   jsonString(obj, "base"),
		Head:                   jsonString(obj, "head"),
		Status:                 jsonStringOr(obj, "status", "open"),
		Ts:                     jsonLongLong(obj, "ts"),
		Author:                 jsonString(obj, "author"),
		AuthorName:             jsonString(obj, "authorName"),
		Sig:                    jsonString(obj, "sig"),
		Patch:                  jsonString(obj, "patch"),
		Commits:                jsonString(obj, "commits"),
		CreationBaseOid:        jsonString(obj, "creationBaseOid"),
		CreationHeadOid:        jsonString(obj, "creationHeadOid"),
		Client:                 jsonString(obj, "client"),
		ClientSig:              jsonString(obj, "clientSig"),
		RewardEstimateLamports: int64(jsonDouble(obj, "rewardEstimateLamports")),
		RewardRecipient:        jsonString(obj, "rewardRecipient"),
		RewardID:               jsonString(obj, "rewardId"),
		RewardCondition:        jsonString(obj, "rewardCondition"),
		RewardStatus:           jsonString(obj, "rewardStatus"),
		RewardTxSignature:      jsonString(obj, "rewardTxSignature"),
	}
	if _, ok := obj["description"]; ok {
		pr.Description = jsonString(obj, "description")
	} else {
		pr.Description = jsonString(obj, "body")
	}
	ComputeStats(&pr)
	return pr
}

func (p *Pull) FullJSON() map[string]any {
	out := p.WireJSON()
	events := make([]any, 0, len(p.Events))
	for i := range p.Events {
		events = append(events, p.Events[i].ToJSON())
	}
	out["events"] = events
	out["branchBacked"] = p.BranchBacked
	out["mergeBase"] = p.MergeBase
	out["mergeHead"] = p.MergeHead
	out["filesChanged"] = p.FilesChanged
	out["additions"] = p.Additions
	out["deletions"] = p.Deletions
	out["patchHeadOid"] = p.PatchHeadOid
	out["archivedOnCodeBranch"] = p.ArchivedOnCodeBranch
	return out
}

func PullFromFullJSON(obj map[string]any) Pull {
	pr := PullFromJSON(obj)
	pr.BranchBacked = jsonBool(obj, "branchBacked")
	pr.MergeBase = jsonString(obj, "mergeBase")
	pr.MergeHead = jsonString(obj, "mergeHead")
	pr.PatchHeadOid = jsonString(obj, "patchHeadOid")
	pr.ArchivedOnCodeBranch = jsonBool(obj, "archivedOnCodeBranch")
	if list, ok := obj["events"].([]any); ok {
		for _, item := range list {
			if event, ok := item.(map[string]any); ok {
				pr.Events = append(pr.Events, EventFromJSON(event))
			}
		}
	}
	return pr
}
