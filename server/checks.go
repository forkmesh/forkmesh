package mirrornode

// Signed status checks and the required-checks merge gate.
//
// A repository opts in by committing .forkmesh/required-checks.json to its
// base branch. The policy names every check a pull request must pass and the
// runner keys allowed to report each one. A runner reports a result as a
// signed attestation (forkmesh-check-v1) bound to the repository, the exact
// commit it tested, the check, the conclusion, its run id, and the time it
// finished. Attestations travel on the pull-request ledger under
// .forkmesh/pulls/<N>/checks/, but the ledger is only transport: nothing about
// where a file sits or who committed it makes it count. A check satisfies the
// policy only when its newest attestation that verifies under a key the
// policy names for that check says "success" for the head being merged, and
// was issued inside the policy's freshness window.
//
// The policy is read from the base commit the owner reviewed, never from the
// pull request's head, so a proposal cannot loosen the gate it is judged by.

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"math/big"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

const (
	requiredChecksPolicyPath   = ".forkmesh/required-checks.json"
	requiredChecksPolicySchema = "forkmesh-required-checks-v1"
	checkAttestationSchema     = "forkmesh-check-v1"
	checkAttestationPrefix     = "forkmesh-check-v1"
	maxRequiredChecksPolicy    = 64 << 10
	maxCheckAttestationBytes   = 8 << 10
	maxCheckAttestations       = 256
	maxRequiredChecks          = 32
	maxCheckSigners            = 16
	minCheckMaxAgeSeconds      = 60
	maxCheckMaxAgeSeconds      = 366 * 24 * 60 * 60
	// The same clock allowance FM-AUTH-001 gives time-bound request proofs.
	checkFutureSkew = 300 * time.Second
)

var (
	checkRunIDPattern    = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$`)
	checkIssuedAtPattern = regexp.MustCompile(`^(?:0|[1-9][0-9]{0,15})$`)
	checkFilePattern     = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,122}\.json$`)
	checkConclusions     = map[string]bool{
		"success": true, "failure": true, "cancelled": true, "skipped": true,
	}
)

// RequiredChecksPolicy is the owner-authored gate committed on the base branch.
type RequiredChecksPolicy struct {
	Schema string `json:"schema"`
	// Base branches the gate applies to; empty applies it to every base.
	Branches      []string        `json:"branches"`
	MaxAgeSeconds int64           `json:"maxAgeSeconds"`
	Checks        []RequiredCheck `json:"checks"`
}

// RequiredCheck names one check and the only runner keys that may report it.
type RequiredCheck struct {
	Name    string   `json:"name"`
	Signers []string `json:"signers"`
}

// CheckAttestation is one runner's signed report of one check on one commit.
type CheckAttestation struct {
	Schema     string      `json:"schema"`
	Owner      string      `json:"owner"`
	Repository string      `json:"repository"`
	Commit     string      `json:"commit"`
	Check      string      `json:"check"`
	Conclusion string      `json:"conclusion"`
	RunID      string      `json:"runId"`
	IssuedAt   json.Number `json:"issuedAt"`
	Signer     string      `json:"signer"`
	Signature  string      `json:"signature"`

	issuedAtMs int64
}

func decodeStrictJSON(raw []byte, limit int, value any) error {
	if len(raw) == 0 || len(raw) > limit || !utf8.Valid(raw) {
		return errors.New("invalid size or encoding")
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	decoder.UseNumber()
	if err := decoder.Decode(value); err != nil {
		return errors.New("invalid JSON")
	}
	if decoder.Decode(&struct{}{}) != io.EOF {
		return errors.New("trailing JSON")
	}
	return nil
}

func validCheckName(name string) bool {
	if name == "" || len(name) > 128 || !utf8.ValidString(name) || strings.TrimSpace(name) != name {
		return false
	}
	for _, r := range name {
		if unicode.IsControl(r) {
			return false
		}
	}
	return true
}

// ParseRequiredChecksPolicy accepts only a complete, well-formed policy. A
// policy that fails to parse must lock merges, never silently disable them.
func ParseRequiredChecksPolicy(raw []byte) (*RequiredChecksPolicy, error) {
	var policy RequiredChecksPolicy
	if err := decodeStrictJSON(raw, maxRequiredChecksPolicy, &policy); err != nil {
		return nil, err
	}
	if policy.Schema != requiredChecksPolicySchema {
		return nil, errors.New("unsupported policy schema")
	}
	if policy.MaxAgeSeconds < minCheckMaxAgeSeconds || policy.MaxAgeSeconds > maxCheckMaxAgeSeconds {
		return nil, errors.New("maxAgeSeconds is out of range")
	}
	seenBranch := map[string]bool{}
	for _, branch := range policy.Branches {
		if validPullBranch(branch) != branch || seenBranch[branch] {
			return nil, errors.New("invalid policy branch")
		}
		seenBranch[branch] = true
	}
	if len(policy.Checks) == 0 || len(policy.Checks) > maxRequiredChecks {
		return nil, errors.New("a policy names 1-32 checks")
	}
	seenCheck := map[string]bool{}
	for _, check := range policy.Checks {
		if !validCheckName(check.Name) || seenCheck[check.Name] {
			return nil, errors.New("invalid or duplicate check name")
		}
		seenCheck[check.Name] = true
		if len(check.Signers) == 0 || len(check.Signers) > maxCheckSigners {
			return nil, errors.New("a check names 1-16 signers")
		}
		seenSigner := map[string]bool{}
		for _, signer := range check.Signers {
			key, err := decodeCanonicalBase64(signer, ed25519.PublicKeySize)
			if err != nil || !strictEd25519PublicKey(key) || seenSigner[signer] {
				return nil, errors.New("invalid or duplicate signer key")
			}
			seenSigner[signer] = true
		}
	}
	return &policy, nil
}

// appliesTo reports whether merges into base are gated by this policy.
func (p *RequiredChecksPolicy) appliesTo(base string) bool {
	if len(p.Branches) == 0 {
		return true
	}
	for _, branch := range p.Branches {
		if branch == base {
			return true
		}
	}
	return false
}

func (a *CheckAttestation) validateFields() error {
	if a.Schema != checkAttestationSchema {
		return errors.New("unsupported attestation schema")
	}
	if !nodePattern.MatchString(a.Owner) || strings.ToLower(a.Owner) != a.Owner ||
		!repositoryPattern.MatchString(a.Repository) {
		return errors.New("invalid attestation repository")
	}
	if !gitObjectIDPattern.MatchString(a.Commit) {
		return errors.New("invalid attestation commit")
	}
	if !validCheckName(a.Check) || !checkConclusions[a.Conclusion] ||
		!checkRunIDPattern.MatchString(a.RunID) {
		return errors.New("invalid attestation check")
	}
	issued := a.IssuedAt.String()
	if !checkIssuedAtPattern.MatchString(issued) {
		return errors.New("invalid attestation issuedAt")
	}
	value, err := strconv.ParseInt(issued, 10, 64)
	if err != nil {
		return errors.New("invalid attestation issuedAt")
	}
	a.issuedAtMs = value
	return nil
}

// signingBytes is the FM-ENC-007 canonical string. The check name is free
// text, so it is bound by digest rather than inline.
func (a *CheckAttestation) signingBytes() []byte {
	name := sha256.Sum256([]byte(a.Check))
	return []byte(strings.Join([]string{
		checkAttestationPrefix, a.Owner, a.Repository, a.Commit,
		hex.EncodeToString(name[:]), a.Conclusion, a.RunID, a.IssuedAt.String(),
	}, "\n"))
}

// ParseCheckAttestation parses and signature-verifies one attestation. It
// proves only who said what about which commit; ChecksSatisfied decides
// whether that speaker is allowed to say it and whether it is still current.
func ParseCheckAttestation(raw []byte) (*CheckAttestation, error) {
	var attestation CheckAttestation
	if err := decodeStrictJSON(raw, maxCheckAttestationBytes, &attestation); err != nil {
		return nil, err
	}
	if err := attestation.validateFields(); err != nil {
		return nil, err
	}
	key, err := decodeCanonicalBase64(attestation.Signer, ed25519.PublicKeySize)
	if err != nil || !strictEd25519PublicKey(key) {
		return nil, errors.New("invalid attestation signer")
	}
	signature, err := decodeCanonicalBase64(attestation.Signature, ed25519.SignatureSize)
	if err != nil || !ed25519.Verify(ed25519.PublicKey(key), attestation.signingBytes(), signature) {
		return nil, errors.New("invalid attestation signature")
	}
	return &attestation, nil
}

// SignCheckAttestation stamps issuedAt from the signer's own clock and signs
// the attestation with the runner identity.
func SignCheckAttestation(identity *Identity, attestation CheckAttestation, now time.Time) (CheckAttestation, error) {
	attestation.Schema = checkAttestationSchema
	attestation.Owner = strings.ToLower(strings.TrimSpace(attestation.Owner))
	attestation.Commit = strings.ToLower(strings.TrimSpace(attestation.Commit))
	attestation.IssuedAt = json.Number(strconv.FormatInt(now.UnixMilli(), 10))
	attestation.Signer = identity.PublicKey()
	if err := attestation.validateFields(); err != nil {
		return attestation, err
	}
	attestation.Signature = identity.Sign(attestation.signingBytes())
	return attestation, nil
}

// RunCheckSigner reads one unsigned attestation (schema, owner, repository,
// commit, check, conclusion, runId) and writes it signed by this node.
func RunCheckSigner(input io.Reader, output io.Writer, identity *Identity) error {
	raw, err := io.ReadAll(io.LimitReader(input, maxCheckAttestationBytes+1))
	if err != nil {
		return errors.New("invalid request size")
	}
	var request struct {
		Schema     string `json:"schema"`
		Owner      string `json:"owner"`
		Repository string `json:"repository"`
		Commit     string `json:"commit"`
		Check      string `json:"check"`
		Conclusion string `json:"conclusion"`
		RunID      string `json:"runId"`
	}
	if err := decodeStrictJSON(raw, maxCheckAttestationBytes, &request); err != nil {
		return err
	}
	if request.Schema != checkAttestationSchema {
		return errors.New("unsupported attestation schema")
	}
	signed, err := SignCheckAttestation(identity, CheckAttestation{
		Owner: request.Owner, Repository: request.Repository, Commit: request.Commit,
		Check: request.Check, Conclusion: request.Conclusion, RunID: request.RunID,
	}, time.Now())
	if err != nil {
		return err
	}
	return json.NewEncoder(output).Encode(signed)
}

// ChecksSatisfied reports whether every check the policy requires has, as its
// newest admissible attestation, a success for exactly this repository and
// commit. It returns the required checks that are not satisfied, in policy
// order. An attestation is inadmissible — ignored, never an error — when it
// does not parse or verify (forged), names another repository or commit
// (replayed), is signed by a key the policy does not name for that check
// (unauthorized), or was issued outside the freshness window (stale).
func ChecksSatisfied(policy *RequiredChecksPolicy, owner, repository, commit string, now time.Time, attestations [][]byte) (bool, []string) {
	signers := map[string]map[string]bool{}
	for _, check := range policy.Checks {
		signers[check.Name] = map[string]bool{}
		for _, signer := range check.Signers {
			signers[check.Name][signer] = true
		}
	}
	nowMs := now.UnixMilli()
	oldest := nowMs - policy.MaxAgeSeconds*1000
	newest := nowMs + checkFutureSkew.Milliseconds()
	latest := map[string]*CheckAttestation{}
	for i, raw := range attestations {
		if i >= maxCheckAttestations {
			break
		}
		attestation, err := ParseCheckAttestation(raw)
		if err != nil || attestation.Owner != owner || attestation.Repository != repository ||
			attestation.Commit != commit || !signers[attestation.Check][attestation.Signer] ||
			attestation.issuedAtMs < oldest || attestation.issuedAtMs > newest {
			continue
		}
		current := latest[attestation.Check]
		// A re-run supersedes an earlier result, so replaying an old success
		// cannot mask a newer failure. On an exact tie the non-success wins.
		if current == nil || attestation.issuedAtMs > current.issuedAtMs ||
			(attestation.issuedAtMs == current.issuedAtMs && attestation.Conclusion != "success") {
			latest[attestation.Check] = attestation
		}
	}
	var missing []string
	for _, check := range policy.Checks {
		if result := latest[check.Name]; result == nil || result.Conclusion != "success" {
			missing = append(missing, check.Name)
		}
	}
	return len(missing) == 0, missing
}

// Ed25519 public-key strictness (FM-SIG-001). crypto/ed25519 accepts
// non-canonical and small- or mixed-order keys, so a key admitted into a
// policy or presented by a signer is decompressed and checked here: y < p, a
// real square root, no negative zero, not the identity, and [L]A = identity.

var (
	edP      = new(big.Int).Sub(new(big.Int).Lsh(big.NewInt(1), 255), big.NewInt(19))
	edL, _   = new(big.Int).SetString("7237005577332262213973186563042994240857116359379907606001950938285454250989", 10)
	edD      = new(big.Int).Mod(new(big.Int).Mul(big.NewInt(-121665), new(big.Int).ModInverse(big.NewInt(121666), edP)), edP)
	edSqrtM1 = new(big.Int).Exp(big.NewInt(2), new(big.Int).Rsh(new(big.Int).Sub(edP, big.NewInt(1)), 2), edP)
	edOne    = big.NewInt(1)
)

func edMod(value *big.Int) *big.Int { return value.Mod(value, edP) }

func edDecompress(encoded []byte) (*big.Int, *big.Int, bool) {
	if len(encoded) != 32 {
		return nil, nil, false
	}
	sign := uint(encoded[31] >> 7)
	bigEndian := make([]byte, 32)
	for i := range encoded {
		bigEndian[31-i] = encoded[i]
	}
	bigEndian[0] &= 0x7f
	y := new(big.Int).SetBytes(bigEndian)
	if y.Cmp(edP) >= 0 {
		return nil, nil, false
	}
	y2 := edMod(new(big.Int).Mul(y, y))
	u := edMod(new(big.Int).Sub(y2, edOne))
	v := edMod(new(big.Int).Add(new(big.Int).Mul(edD, y2), edOne))
	x2 := edMod(new(big.Int).Mul(u, new(big.Int).ModInverse(v, edP)))
	x := new(big.Int).Exp(x2, new(big.Int).Rsh(new(big.Int).Add(edP, big.NewInt(3)), 3), edP)
	if edMod(new(big.Int).Mul(x, x)).Cmp(x2) != 0 {
		x = edMod(x.Mul(x, edSqrtM1))
		if edMod(new(big.Int).Mul(x, x)).Cmp(x2) != 0 {
			return nil, nil, false
		}
	}
	if x.Sign() == 0 && sign == 1 {
		return nil, nil, false
	}
	if x.Bit(0) != sign {
		x.Sub(edP, x)
	}
	return x, y, true
}

// edAdd is the complete affine addition law for a = -1 twisted Edwards curves.
func edAdd(x1, y1, x2, y2 *big.Int) (*big.Int, *big.Int) {
	xx := edMod(new(big.Int).Mul(x1, x2))
	yy := edMod(new(big.Int).Mul(y1, y2))
	dxy := edMod(new(big.Int).Mul(edD, new(big.Int).Mul(xx, yy)))
	xNum := edMod(new(big.Int).Add(new(big.Int).Mul(x1, y2), new(big.Int).Mul(y1, x2)))
	yNum := edMod(new(big.Int).Add(yy, xx))
	xDen := new(big.Int).ModInverse(edMod(new(big.Int).Add(edOne, dxy)), edP)
	yDen := new(big.Int).ModInverse(edMod(new(big.Int).Sub(edOne, dxy)), edP)
	return edMod(xNum.Mul(xNum, xDen)), edMod(yNum.Mul(yNum, yDen))
}

func strictEd25519PublicKey(key []byte) bool {
	x, y, ok := edDecompress(key)
	if !ok || (x.Sign() == 0 && y.Cmp(edOne) == 0) {
		return false
	}
	rx, ry := big.NewInt(0), big.NewInt(1)
	for i := edL.BitLen() - 1; i >= 0; i-- {
		rx, ry = edAdd(rx, ry, rx, ry)
		if edL.Bit(i) == 1 {
			rx, ry = edAdd(rx, ry, x, y)
		}
	}
	return rx.Sign() == 0 && ry.Cmp(edOne) == 0
}
