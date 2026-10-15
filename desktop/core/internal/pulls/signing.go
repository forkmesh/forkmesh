package pulls

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/identity"
)

func RepoKey(owner, name string) string {
	o := strings.ToLower(strings.TrimSpace(owner))
	n := strings.ToLower(strings.TrimSpace(name))
	if o == "" || n == "" {
		return ""
	}
	for _, part := range []string{o, n} {
		if strings.ContainsAny(part, "/\n\x00") {
			return ""
		}
	}
	return o + "/" + n
}

func signingPrefix(kind, repoKey string) string {
	if repoKey == "" {
		return "forkmesh-" + kind + "-v1\n"
	}
	return "forkmesh-" + kind + "-v2\n" + repoKey + "\n"
}

func sha256Hex(text string) string {
	sum := sha256.Sum256([]byte(text))
	return hex.EncodeToString(sum[:])
}

func (e *Event) HasAnchor() bool { return e.Patchset != "" || e.AnchorLines != "" }

func ContentForSigning(ev *Event) string {
	const nul = "\x00"
	switch ev.Type {
	case "comment":
		return ev.Body
	case "review":
		return ev.State + nul + ev.Body
	}
	anchor := ""
	if ev.HasAnchor() {
		anchor = nul + ev.Patchset + nul + ev.AnchorLines
	}
	switch ev.Type {
	case "line-comment":
		return ev.Path + nul + ev.Side + nul + strconv.Itoa(ev.Line) + nul + ev.Body + anchor
	case "thread-comment":
		return ev.ThreadID + nul + ev.Path + nul + ev.Side + nul + strconv.Itoa(ev.LineStart) + nul +
			strconv.Itoa(ev.LineEnd) + nul + ev.Body + nul + ev.SuggestionPatch + anchor
	case "thread-reply":
		return ev.ThreadID + nul + ev.ParentID + nul + ev.Body
	case "thread-state":
		return ev.ThreadID + nul + ev.State + nul + ev.Body
	case "suggestion-state":
		return ev.ThreadID + nul + ev.State + nul + ev.AppliedCommit + nul + ev.Body
	}
	return ""
}

func EventCanonical(number int, ev *Event, repoKey string) []byte {
	return []byte(signingPrefix("pull-comment", repoKey) + ev.Type + "\n" + strconv.Itoa(number) + "\n" +
		ev.Author + "\n" + strconv.FormatInt(ev.Ts, 10) + "\n" + sha256Hex(ContentForSigning(ev)))
}

func BranchEventCanonical(branch string, ev *Event, repoKey string) []byte {
	return []byte(signingPrefix("branch-comment", repoKey) + ev.Type + "\n" + branch + "\n" +
		ev.Author + "\n" + strconv.FormatInt(ev.Ts, 10) + "\n" + sha256Hex(ContentForSigning(ev)))
}

func PullCanonical(pr *Pull, repoKey string) []byte {
	const nul = "\x00"
	content := pr.Title + nul + pr.Base + nul + pr.Head + nul + pr.Patch + nul + pr.Commits
	return []byte(signingPrefix("pull-event", repoKey) + pr.Author + "\n" + strconv.FormatInt(pr.Ts, 10) + "\n" + sha256Hex(content))
}

func LegacyPullCanonical(pr *Pull) []byte {
	const nul = "\x00"
	content := pr.Title + nul + pr.Base + nul + pr.Head + nul + pr.Patch
	return []byte("forkmesh-pull-event-v1\n" + pr.Author + "\n" + strconv.FormatInt(pr.Ts, 10) + "\n" + sha256Hex(content))
}

func NewID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		now := time.Now().UnixNano()
		for i := range b {
			b[i] = byte(now >> (uint(i%8) * 8))
		}
	}
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

type Signer struct {
	Identity   *identity.Identity
	AuthorName string
	RepoKey    string
	Now        func() time.Time
}

func (s *Signer) nowMs() int64 {
	if s.Now != nil {
		return s.Now().UnixMilli()
	}
	return time.Now().UnixMilli()
}

func (s *Signer) publicKey() string {
	if s == nil || s.Identity == nil {
		return ""
	}
	return s.Identity.PublicKey()
}

func (s *Signer) sign(data []byte) string {
	if s == nil || s.Identity == nil {
		return ""
	}
	return s.Identity.Sign(data)
}

func (s *Signer) prepare(ev Event) Event {
	if ev.ID == "" {
		ev.ID = NewID()
	}
	if ev.Type == "thread-comment" && ev.ThreadID == "" {
		ev.ThreadID = ev.ID
	}
	ev.Author = s.publicKey()
	if ev.AuthorName == "" && s != nil {
		ev.AuthorName = s.AuthorName
	}
	if ev.Ts == 0 {
		ev.Ts = s.nowMs()
	}
	return ev
}

func (s *Signer) SignEvent(number int, ev Event) Event {
	ev = s.prepare(ev)
	repoKey := ""
	if s != nil {
		repoKey = s.RepoKey
	}
	ev.Sig = s.sign(EventCanonical(number, &ev, repoKey))
	return ev
}

func (s *Signer) SignBranchEvent(branch string, ev Event) Event {
	ev = s.prepare(ev)
	repoKey := ""
	if s != nil {
		repoKey = s.RepoKey
	}
	ev.Sig = s.sign(BranchEventCanonical(branch, &ev, repoKey))
	return ev
}

func (s *Signer) SignPull(pr Pull) Pull {
	pr.Author = s.publicKey()
	if pr.AuthorName == "" && s != nil {
		pr.AuthorName = s.AuthorName
	}
	if pr.Ts == 0 {
		pr.Ts = s.nowMs()
	}
	repoKey := ""
	if s != nil {
		repoKey = s.RepoKey
	}
	pr.Sig = s.sign(PullCanonical(&pr, repoKey))
	return pr
}

func encodeFrontMatterBlob(text string) string {
	if text == "" {
		return ""
	}
	return base64.RawURLEncoding.EncodeToString([]byte(text))
}

func stripEdgeNewlines(text string) string {
	return strings.TrimRight(strings.TrimLeft(text, "\n"), "\n")
}
