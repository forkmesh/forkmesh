package mirrornode

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	agentModelGrantPrefix   = "forkmesh-agent-model-grant-v1"
	agentCommitPrefix       = "forkmesh-agent-commit-v1"
	agentAttestationTrailer = "ForkMesh-Attestation: "
	agentModelTrailer       = "ForkMesh-Agent: "
	maxAgentModelGrant      = 4096
	maxAttestedCommitBytes  = 1 << 20
)

var (
	agentModelPattern   = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$`)
	agentLeasePattern   = regexp.MustCompile(`^[A-Za-z0-9_-]{8,80}$`)
	agentGrantProviders = map[string]bool{"claude-code": true, "codex": true}
)

type agentModelGrant struct {
	Runner     string
	RunnerKey  string
	Owner      string
	Repository string
	Ref        string
	Provider   string
	Model      string
	LeaseID    string
	IssuedAt   int64
	ExpiresAt  int64
}

func agentModelGrantMessage(grant agentModelGrant) string {
	return strings.Join([]string{
		agentModelGrantPrefix, grant.Runner, grant.RunnerKey, grant.Owner, grant.Repository,
		grant.Ref, grant.Provider, grant.Model, grant.LeaseID,
		strconv.FormatInt(grant.IssuedAt, 10), strconv.FormatInt(grant.ExpiresAt, 10),
	}, "\n")
}

func parseAgentModelGrantMessage(message string) (agentModelGrant, bool) {
	fields := strings.Split(message, "\n")
	if len(fields) != 11 || fields[0] != agentModelGrantPrefix {
		return agentModelGrant{}, false
	}
	issued, errIssued := strconv.ParseInt(fields[9], 10, 64)
	expires, errExpires := strconv.ParseInt(fields[10], 10, 64)
	grant := agentModelGrant{
		Runner: fields[1], RunnerKey: fields[2], Owner: fields[3], Repository: fields[4],
		Ref: fields[5], Provider: fields[6], Model: fields[7], LeaseID: fields[8],
		IssuedAt: issued, ExpiresAt: expires,
	}
	key, err := decodeCanonicalBase64(grant.RunnerKey, ed25519.PublicKeySize)
	if err != nil || errIssued != nil || errExpires != nil || issued <= 0 || expires <= issued ||
		strconv.FormatInt(issued, 10) != fields[9] || strconv.FormatInt(expires, 10) != fields[10] ||
		!nodePattern.MatchString(grant.Runner) || !strictEd25519PublicKey(key) ||
		!nodePattern.MatchString(grant.Owner) || !repositoryPattern.MatchString(grant.Repository) ||
		!validRunnerAgentRef(grant.Ref) || !agentGrantProviders[grant.Provider] ||
		!agentModelPattern.MatchString(grant.Model) || !agentLeasePattern.MatchString(grant.LeaseID) {
		return agentModelGrant{}, false
	}
	return grant, true
}

func verifyAgentModelGrant(token, routerPublicKey string) (agentModelGrant, bool) {
	encoded, signature, found := strings.Cut(token, ".")
	if !found || len(token) > maxAgentModelGrant {
		return agentModelGrant{}, false
	}
	routerKey, err := decodeCanonicalBase64(routerPublicKey, ed25519.PublicKeySize)
	if err != nil {
		return agentModelGrant{}, false
	}
	raw, err := decodeCanonicalBase64(encoded, -1)
	if err != nil || !utf8.Valid(raw) {
		return agentModelGrant{}, false
	}
	sig, err := decodeCanonicalBase64(signature, ed25519.SignatureSize)
	if err != nil || !ed25519.Verify(ed25519.PublicKey(routerKey), raw, sig) {
		return agentModelGrant{}, false
	}
	return parseAgentModelGrantMessage(string(raw))
}

func parseAgentModelGrantToken(token string) (agentModelGrant, bool) {
	encoded, _, found := strings.Cut(token, ".")
	if !found || len(token) > maxAgentModelGrant {
		return agentModelGrant{}, false
	}
	raw, err := base64.RawURLEncoding.DecodeString(encoded)
	if err != nil || !utf8.Valid(raw) {
		return agentModelGrant{}, false
	}
	return parseAgentModelGrantMessage(string(raw))
}

type rawCommit struct {
	tree    string
	parents []string
	headers []string
	message string
}

func parseRawCommit(raw []byte) (rawCommit, bool) {
	text := string(raw)
	header, message, found := strings.Cut(text, "\n\n")
	if !found {
		if !strings.HasSuffix(text, "\n") {
			return rawCommit{}, false
		}
		header, message = strings.TrimSuffix(text, "\n"), ""
	}
	var commit rawCommit
	for index, line := range strings.Split(header, "\n") {
		switch {
		case index == 0:
			tree, ok := strings.CutPrefix(line, "tree ")
			if !ok || !gitObjectIDPattern.MatchString(tree) {
				return rawCommit{}, false
			}
			commit.tree = tree
		case strings.HasPrefix(line, "parent ") && len(commit.headers) == 0:
			parent := strings.TrimPrefix(line, "parent ")
			if !gitObjectIDPattern.MatchString(parent) {
				return rawCommit{}, false
			}
			commit.parents = append(commit.parents, parent)
		default:
			commit.headers = append(commit.headers, line)
		}
	}
	if commit.tree == "" {
		return rawCommit{}, false
	}
	commit.message = message
	return commit, true
}

func (c rawCommit) encode() []byte {
	var builder strings.Builder
	builder.WriteString("tree " + c.tree + "\n")
	for _, parent := range c.parents {
		builder.WriteString("parent " + parent + "\n")
	}
	for _, header := range c.headers {
		builder.WriteString(header + "\n")
	}
	builder.WriteString("\n")
	builder.WriteString(c.message)
	return []byte(builder.String())
}

func (c rawCommit) subject() string {
	subject, _, _ := strings.Cut(strings.TrimLeft(c.message, "\n"), "\n")
	return strings.TrimSpace(subject)
}

func withoutSignatureHeaders(headers []string) []string {
	var kept []string
	skipping := false
	for _, header := range headers {
		if skipping && strings.HasPrefix(header, " ") {
			continue
		}
		skipping = strings.HasPrefix(header, "gpgsig ") || strings.HasPrefix(header, "gpgsig-sha256 ")
		if !skipping {
			kept = append(kept, header)
		}
	}
	return kept
}

func agentCommitSigningBytes(tree string, parents []string, prefix, grantToken string) []byte {
	message := sha256.Sum256([]byte(prefix))
	grant := sha256.Sum256([]byte(grantToken))
	return []byte(strings.Join([]string{
		agentCommitPrefix, tree, strings.Join(parents, " "),
		hex.EncodeToString(message[:]), hex.EncodeToString(grant[:]),
	}, "\n"))
}

func attestAgentCommit(raw []byte, parents []string, grantToken string, grant agentModelGrant, identity *Identity) ([]byte, error) {
	commit, ok := parseRawCommit(raw)
	if !ok || len(parents) != len(commit.parents) {
		return nil, errors.New("commit object is invalid")
	}
	commit.parents = parents
	commit.headers = withoutSignatureHeaders(commit.headers)
	lines := strings.Split(strings.TrimRight(commit.message, "\n"), "\n")
	for len(lines) > 0 && (strings.HasPrefix(lines[len(lines)-1], agentAttestationTrailer) ||
		strings.HasPrefix(lines[len(lines)-1], agentModelTrailer)) {
		lines = lines[:len(lines)-1]
	}
	body := strings.TrimRight(strings.Join(lines, "\n"), "\n")
	prefix := agentModelTrailer + grant.Provider + "/" + grant.Model + "\n"
	if body != "" {
		prefix = body + "\n\n" + prefix
	}
	signature := identity.Sign(agentCommitSigningBytes(commit.tree, commit.parents, prefix, grantToken))
	commit.message = prefix + agentAttestationTrailer + grantToken + "." + signature + "\n"
	return commit.encode(), nil
}

func verifyAgentCommit(commit rawCommit, routerPublicKey string) (agentModelGrant, bool) {
	message := commit.message
	if !strings.HasSuffix(message, "\n") {
		return agentModelGrant{}, false
	}
	start := strings.LastIndex(message[:len(message)-1], "\n") + 1
	token, found := strings.CutPrefix(message[start:len(message)-1], agentAttestationTrailer)
	if !found {
		return agentModelGrant{}, false
	}
	cut := strings.LastIndex(token, ".")
	if cut <= 0 {
		return agentModelGrant{}, false
	}
	grantToken, signature := token[:cut], token[cut+1:]
	grant, ok := verifyAgentModelGrant(grantToken, routerPublicKey)
	if !ok {
		return agentModelGrant{}, false
	}
	key, _ := decodeCanonicalBase64(grant.RunnerKey, ed25519.PublicKeySize)
	sig, err := decodeCanonicalBase64(signature, ed25519.SignatureSize)
	if err != nil || !ed25519.Verify(ed25519.PublicKey(key),
		agentCommitSigningBytes(commit.tree, commit.parents, message[:start], grantToken), sig) {
		return agentModelGrant{}, false
	}
	return grant, true
}
