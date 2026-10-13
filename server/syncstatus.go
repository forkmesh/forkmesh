package mirrornode

// Human-readable synchronization status (forkmesh-sync-status-v1).
//
// An independent implementation of conformance/sync-status/v1/model.json; the
// shared fixture in that directory pins it to the Worker's reference
// (app/src/sync_status.py) case for case. The output is composed only from
// counts, timestamps, and closed vocabularies, so node names, peer addresses,
// and raw error text never reach the summary.

import (
	"fmt"
	"strings"
	"time"
)

const (
	SyncStatusSchema     = "forkmesh-sync-status-v1"
	syncStatusStaleAfter = int64(60 * 60 * 1000)
	syncStatusDeadline   = int64(15 * 60 * 1000)
)

var syncStatusLabels = map[string]string{
	"queued":        "Queued",
	"syncing":       "Syncing",
	"verified":      "Verified",
	"stale":         "Stale",
	"degraded":      "Degraded",
	"conflicted":    "Conflicted",
	"paused":        "Paused",
	"external-only": "External only",
	"failed":        "Failed",
}

var syncStatusSeverity = map[string]string{
	"queued":        "info",
	"syncing":       "info",
	"verified":      "ok",
	"stale":         "warning",
	"degraded":      "warning",
	"conflicted":    "error",
	"paused":        "info",
	"external-only": "info",
	"failed":        "error",
}

var syncStatusHeadlines = map[string]string{
	"queued":        "Waiting to synchronize",
	"syncing":       "Synchronizing now",
	"verified":      "Every copy is verified and current",
	"stale":         "The visible state may be out of date",
	"degraded":      "Verified, but some copies are not current",
	"conflicted":    "Copies disagree about the latest state",
	"paused":        "Synchronization is paused",
	"external-only": "Only stored on the external provider so far",
	"failed":        "Synchronization failed",
}

var syncStatusReportedErrors = map[string]bool{
	"rejected": true, "signature_invalid": true, "unauthorized": true,
	"provider_unavailable": true, "network": true, "quota_exceeded": true,
}

var syncStatusActions = map[string]string{
	"conflict":             "Review the conflicting changes and choose which state to keep.",
	"rejected":             "A copy rejected the update; check the node's signing key and push again.",
	"signature_invalid":    "The update's signature did not verify; re-sign it from a trusted device.",
	"unauthorized":         "This node is not authorized to publish; reconnect the account and retry.",
	"provider_unavailable": "The external provider did not respond; ForkMesh retries automatically.",
	"network":              "A network error interrupted synchronization; it retries when the connection returns.",
	"quota_exceeded":       "A service quota was reached; synchronization resumes when the quota resets.",
	"sync_delayed":         "Synchronization is taking longer than expected; check that the source node is online.",
	"unreachable":          "One or more copies could not be reached; check that the mirrors are online.",
	"unverified":           "A copy reports the latest state without a verified signature; wait for it to re-attest.",
	"copies_behind":        "Some copies have not received the latest state yet; they normally catch up within minutes.",
	"confirmation_expired": "No copy has confirmed the state recently; check that the mirrors are online.",
	"unconfirmed":          "No copy has confirmed the latest state yet.",
	"unknown":              "An unexpected error stopped synchronization; see the node's log for details.",
}

// SyncCopy is one copy's evidence. Status is current, behind, unreachable, or
// rejected; anything else counts as unreachable.
type SyncCopy struct {
	Status      string `json:"status"`
	Verified    bool   `json:"verified"`
	Trusted     bool   `json:"trusted"`
	ConfirmedAt *int64 `json:"confirmedAt"`
}

// SyncEvidence is what a client observed about one object.
type SyncEvidence struct {
	Now             int64      `json:"now"`
	Paused          bool       `json:"paused"`
	ExternalOnly    bool       `json:"externalOnly"`
	SourceUpdatedAt *int64     `json:"sourceUpdatedAt"`
	QueuedAt        *int64     `json:"queuedAt"`
	SyncStartedAt   *int64     `json:"syncStartedAt"`
	Conflicts       int64      `json:"conflicts"`
	RequiredCopies  *int64     `json:"requiredCopies"`
	LastError       string     `json:"lastError"`
	Copies          []SyncCopy `json:"copies"`
}

// SyncStatus is the machine-readable and plain-language result.
type SyncStatus struct {
	Schema          string  `json:"schema"`
	State           string  `json:"state"`
	Label           string  `json:"label"`
	Severity        string  `json:"severity"`
	Current         bool    `json:"current"`
	TotalCopies     int     `json:"totalCopies"`
	NodesReached    int     `json:"nodesReached"`
	VerifiedCopies  int     `json:"verifiedCopies"`
	TrustedMirrors  int     `json:"trustedMirrors"`
	LastConfirmedAt *int64  `json:"lastConfirmedAt"`
	LagMs           *int64  `json:"lagMs"`
	Error           *string `json:"error"`
	Action          *string `json:"action"`
	Summary         string  `json:"summary"`
}

func syncStatusPlural(count int, singular, plural string) string {
	if count == 1 {
		return fmt.Sprintf("%d %s", count, singular)
	}
	return fmt.Sprintf("%d %s", count, plural)
}

func syncStatusAge(ms int64) string {
	if minutes := ms / 60000; minutes < 60 {
		return fmt.Sprintf("%d min", minutes)
	}
	if hours := ms / 3600000; hours < 48 {
		return fmt.Sprintf("%d h", hours)
	}
	return fmt.Sprintf("%d d", ms/86400000)
}

func maxOptional(current *int64, value int64) *int64 {
	if current == nil || value > *current {
		v := value
		return &v
	}
	return current
}

// EvaluateSyncStatus applies the forkmesh-sync-status-v1 precedence rules.
func EvaluateSyncStatus(evidence SyncEvidence) SyncStatus {
	now := evidence.Now
	required := int64(1)
	if evidence.RequiredCopies != nil && *evidence.RequiredCopies > 1 {
		required = *evidence.RequiredCopies
	}
	lastError := evidence.LastError
	if lastError != "" && !syncStatusReportedErrors[lastError] {
		lastError = "unknown"
	}

	total := len(evidence.Copies)
	reached, verifiedCurrent, trustedCurrent := 0, 0, 0
	rejected, unreachable, unverified := 0, 0, 0
	var lastConfirmed, currentConfirmed *int64
	for _, copy := range evidence.Copies {
		status := copy.Status
		switch status {
		case "current", "behind", "unreachable", "rejected":
		default:
			status = "unreachable"
		}
		if status != "unreachable" {
			reached++
		}
		switch status {
		case "rejected":
			rejected++
		case "unreachable":
			unreachable++
		}
		if status == "current" && copy.Verified {
			verifiedCurrent++
			if copy.Trusted {
				trustedCurrent++
			}
			if copy.ConfirmedAt != nil {
				currentConfirmed = maxOptional(currentConfirmed, *copy.ConfirmedAt)
			}
		} else if status == "current" {
			unverified++
		}
		if copy.Verified && status != "rejected" && copy.ConfirmedAt != nil {
			lastConfirmed = maxOptional(lastConfirmed, *copy.ConfirmedAt)
		}
	}

	inFlight := evidence.SyncStartedAt != nil
	delayed := inFlight && now-*evidence.SyncStartedAt > syncStatusDeadline

	state, code := "", ""
	switch {
	case evidence.Paused:
		state = "paused"
	case evidence.Conflicts > 0:
		state, code = "conflicted", "conflict"
	case evidence.ExternalOnly && total == 0 && lastError == "":
		state = "external-only"
	case int64(verifiedCurrent) >= required:
		switch {
		case currentConfirmed == nil || now-*currentConfirmed > syncStatusStaleAfter:
			state, code = "stale", "confirmation_expired"
		case verifiedCurrent < total:
			state = "degraded"
			switch {
			case rejected > 0:
				code = "rejected"
			case unreachable > 0:
				code = "unreachable"
			case unverified > 0:
				code = "unverified"
			default:
				code = "copies_behind"
			}
		default:
			state = "verified"
		}
	case inFlight && !delayed:
		state = "syncing"
	case evidence.QueuedAt != nil && !inFlight:
		state = "queued"
	case rejected > 0:
		state, code = "failed", "rejected"
		if lastError != "" {
			code = lastError
		}
	case lastError != "":
		state, code = "failed", lastError
	case delayed:
		state, code = "stale", "sync_delayed"
	case total > 0 && unreachable == total:
		state, code = "stale", "unreachable"
	default:
		state, code = "stale", "unconfirmed"
	}

	var lag *int64
	if state == "verified" {
		zero := int64(0)
		lag = &zero
	} else if evidence.SourceUpdatedAt != nil {
		value := max(0, now-*evidence.SourceUpdatedAt)
		lag = &value
	}

	parts := []string{
		syncStatusHeadlines[state] + ".",
		fmt.Sprintf("%d of %s verified, %s, %s reached.", verifiedCurrent,
			syncStatusPlural(total, "copy", "copies"),
			syncStatusPlural(trustedCurrent, "trusted mirror", "trusted mirrors"),
			syncStatusPlural(reached, "node", "nodes")),
	}
	switch {
	case lastConfirmed == nil:
		parts = append(parts, "Never confirmed.")
	case now-*lastConfirmed < 60000:
		parts = append(parts, "Last confirmed just now.")
	default:
		parts = append(parts, "Last confirmed "+syncStatusAge(now-*lastConfirmed)+" ago.")
	}
	if lag != nil && *lag >= 60000 {
		parts = append(parts, syncStatusAge(*lag)+" behind the source.")
	}
	result := SyncStatus{
		Schema:          SyncStatusSchema,
		State:           state,
		Label:           syncStatusLabels[state],
		Severity:        syncStatusSeverity[state],
		Current:         state == "verified" || state == "degraded",
		TotalCopies:     total,
		NodesReached:    reached,
		VerifiedCopies:  verifiedCurrent,
		TrustedMirrors:  trustedCurrent,
		LastConfirmedAt: lastConfirmed,
		LagMs:           lag,
	}
	if code != "" {
		action := syncStatusActions[code]
		result.Error = &code
		result.Action = &action
		parts = append(parts, action)
	}
	result.Summary = strings.Join(parts, " ")
	return result
}

// daemonSyncEvidence describes this mirror node's own copy: the last sync
// cycle either served current, verified refs or it did not. The raw failure
// text stays in lastSyncError; the status reports only the closed "unknown"
// code so it can be shown without leaking peer addresses.
func daemonSyncEvidence(now, startedAt, lastSyncAt time.Time, lastSyncOK bool, lastSyncErr string) SyncEvidence {
	evidence := SyncEvidence{Now: now.UnixMilli()}
	if lastSyncAt.IsZero() {
		queued := startedAt.UnixMilli()
		evidence.QueuedAt = &queued
		return evidence
	}
	confirmed := lastSyncAt.UnixMilli()
	copy := SyncCopy{Status: "current", Verified: true, Trusted: true, ConfirmedAt: &confirmed}
	if !lastSyncOK {
		copy = SyncCopy{Status: "behind", ConfirmedAt: &confirmed}
		if lastSyncErr != "" {
			evidence.LastError = "unknown"
		}
	}
	evidence.Copies = []SyncCopy{copy}
	return evidence
}
