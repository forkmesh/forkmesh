package mirrornode

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"strings"
	"testing"
	"time"
)

var checkTestNow = time.UnixMilli(1_800_000_000_000)

func newRunnerIdentity(t *testing.T) *Identity {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return &Identity{private: private, public: public}
}

func checkPolicyJSON(t *testing.T, maxAge int64, branches []string, checks map[string][]*Identity) []byte {
	t.Helper()
	policy := RequiredChecksPolicy{Schema: requiredChecksPolicySchema, Branches: branches, MaxAgeSeconds: maxAge}
	if policy.Branches == nil {
		policy.Branches = []string{}
	}
	for _, name := range []string{"CI tests", "Desktop client CI tests"} {
		signers, ok := checks[name]
		if !ok {
			continue
		}
		check := RequiredCheck{Name: name}
		for _, signer := range signers {
			check.Signers = append(check.Signers, signer.PublicKey())
		}
		policy.Checks = append(policy.Checks, check)
	}
	raw, err := json.Marshal(policy)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

type checkReport struct {
	owner, repository, commit, check, conclusion, runID string
	at                                                  time.Time
}

func signedCheck(t *testing.T, signer *Identity, report checkReport) []byte {
	t.Helper()
	signed, err := SignCheckAttestation(signer, CheckAttestation{
		Owner: report.owner, Repository: report.repository, Commit: report.commit,
		Check: report.check, Conclusion: report.conclusion, RunID: report.runID,
	}, report.at)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := json.Marshal(signed)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestRequiredChecksRejectForgedStaleReplayedAndUnauthorizedReports(t *testing.T) {
	runner, outsider := newRunnerIdentity(t), newRunnerIdentity(t)
	commit, other := strings.Repeat("a", 40), strings.Repeat("b", 40)
	policy, err := ParseRequiredChecksPolicy(checkPolicyJSON(t, 3600, nil,
		map[string][]*Identity{"CI tests": {runner}}))
	if err != nil {
		t.Fatal(err)
	}
	good := checkReport{"forkmesh", "forkmesh", commit, "CI tests", "success", "run-41", checkTestNow.Add(-time.Minute)}
	satisfied := func(reports ...[]byte) bool {
		ok, missing := ChecksSatisfied(policy, "forkmesh", "forkmesh", commit, checkTestNow, reports)
		if ok != (len(missing) == 0) {
			t.Fatalf("verdict and missing list disagree: %v %v", ok, missing)
		}
		return ok
	}
	if satisfied() {
		t.Fatal("no attestation satisfied the policy")
	}
	if !satisfied(signedCheck(t, runner, good)) {
		t.Fatal("a fresh, authorized success did not satisfy the policy")
	}

	// Forged: a valid report whose conclusion was edited after signing, and a
	// signature swapped in from another report.
	forged := signedCheck(t, runner, checkReport{"forkmesh", "forkmesh", commit, "CI tests", "failure", "run-41", good.at})
	edited := bytes.Replace(forged, []byte(`"conclusion":"failure"`), []byte(`"conclusion":"success"`), 1)
	if satisfied(edited) {
		t.Fatal("an edited conclusion satisfied the policy")
	}
	var swapped, donor CheckAttestation
	_ = json.Unmarshal(signedCheck(t, runner, good), &swapped)
	_ = json.Unmarshal(signedCheck(t, runner, checkReport{"forkmesh", "forkmesh", other, "CI tests", "success", "run-41", good.at}), &donor)
	swapped.Commit = donor.Commit
	if raw, _ := json.Marshal(swapped); satisfied(raw) {
		t.Fatal("a report with a rebound commit satisfied the policy")
	}

	// Unauthorized: well signed, by a key the policy does not name.
	if satisfied(signedCheck(t, outsider, good)) {
		t.Fatal("an unauthorized runner satisfied the policy")
	}
	// An authorized key reporting a check it was not trusted for.
	if satisfied(signedCheck(t, runner, checkReport{"forkmesh", "forkmesh", commit, "Lint", "success", "run-41", good.at})) {
		t.Fatal("an unrequired check name satisfied the policy")
	}

	// Stale and from the future.
	stale := good
	stale.at = checkTestNow.Add(-2 * time.Hour)
	if satisfied(signedCheck(t, runner, stale)) {
		t.Fatal("a stale report satisfied the policy")
	}
	future := good
	future.at = checkTestNow.Add(10 * time.Minute)
	if satisfied(signedCheck(t, runner, future)) {
		t.Fatal("a future-dated report satisfied the policy")
	}

	// Replayed: the same runner's success for another repository, owner, or
	// commit.
	for _, replay := range []checkReport{
		{"forkmesh", "other-repo", commit, "CI tests", "success", "run-41", good.at},
		{"mallory", "forkmesh", commit, "CI tests", "success", "run-41", good.at},
		{"forkmesh", "forkmesh", other, "CI tests", "success", "run-41", good.at},
	} {
		if satisfied(signedCheck(t, runner, replay)) {
			t.Fatalf("a replayed report satisfied the policy: %+v", replay)
		}
	}

	// An old success replayed next to a newer failure does not win, and a tie
	// resolves to the failure.
	failed := good
	failed.conclusion, failed.runID, failed.at = "failure", "run-42", good.at.Add(time.Second)
	if satisfied(signedCheck(t, runner, good), signedCheck(t, runner, failed)) {
		t.Fatal("an older success masked a newer failure")
	}
	tied := good
	tied.conclusion = "cancelled"
	if satisfied(signedCheck(t, runner, tied), signedCheck(t, runner, good)) {
		t.Fatal("a tie resolved to success")
	}
	rerun := good
	rerun.runID, rerun.at = "run-43", failed.at.Add(time.Second)
	if !satisfied(signedCheck(t, runner, failed), signedCheck(t, runner, rerun)) {
		t.Fatal("a newer successful re-run did not supersede the failure")
	}
}

func TestRequiredChecksNeedEveryCheck(t *testing.T) {
	ci, desktop := newRunnerIdentity(t), newRunnerIdentity(t)
	commit := strings.Repeat("c", 64)
	policy, err := ParseRequiredChecksPolicy(checkPolicyJSON(t, 600, []string{"main"},
		map[string][]*Identity{"CI tests": {ci}, "Desktop client CI tests": {desktop}}))
	if err != nil {
		t.Fatal(err)
	}
	if !policy.appliesTo("main") || policy.appliesTo("release") {
		t.Fatal("branch scope not applied")
	}
	at := checkTestNow.Add(-time.Second)
	ok, missing := ChecksSatisfied(policy, "forkmesh", "forkmesh", commit, checkTestNow, [][]byte{
		signedCheck(t, ci, checkReport{"forkmesh", "forkmesh", commit, "CI tests", "success", "a1", at}),
		// Signed by the CI runner, which is not trusted for the desktop check.
		signedCheck(t, ci, checkReport{"forkmesh", "forkmesh", commit, "Desktop client CI tests", "success", "a2", at}),
	})
	if ok || len(missing) != 1 || missing[0] != "Desktop client CI tests" {
		t.Fatalf("partial checks: %v %v", ok, missing)
	}
}

func TestRequiredChecksPolicyFailsClosedOnMalformedInput(t *testing.T) {
	runner := newRunnerIdentity(t)
	valid := string(checkPolicyJSON(t, 600, nil, map[string][]*Identity{"CI tests": {runner}}))
	if _, err := ParseRequiredChecksPolicy([]byte(valid)); err != nil {
		t.Fatal(err)
	}
	for name, raw := range map[string]string{
		"unknown key":     strings.Replace(valid, `"schema"`, `"extra":1,"schema"`, 1),
		"wrong schema":    strings.Replace(valid, requiredChecksPolicySchema, "forkmesh-required-checks-v2", 1),
		"no checks":       `{"schema":"forkmesh-required-checks-v1","branches":[],"maxAgeSeconds":600,"checks":[]}`,
		"age too small":   strings.Replace(valid, `"maxAgeSeconds":600`, `"maxAgeSeconds":1`, 1),
		"bad branch":      strings.Replace(valid, `"branches":[]`, `"branches":["../main"]`, 1),
		"trailing JSON":   valid + "{}",
		"duplicate check": strings.Replace(valid, `}]}`, `},{"name":"CI tests","signers":["`+runner.PublicKey()+`"]}]}`, 1),
		"padded key":      strings.Replace(valid, runner.PublicKey(), runner.PublicKey()+"=", 1),
	} {
		if _, err := ParseRequiredChecksPolicy([]byte(raw)); err == nil {
			t.Fatalf("%s: accepted %s", name, raw)
		}
	}
}

func edCompress(x, y *big.Int) []byte {
	bigEndian := make([]byte, 32)
	y.FillBytes(bigEndian)
	out := make([]byte, 32)
	for i := range bigEndian {
		out[31-i] = bigEndian[i]
	}
	out[31] |= byte(x.Bit(0)) << 7
	return out
}

func TestStrictEd25519KeysRejectWeakEncodings(t *testing.T) {
	runner := newRunnerIdentity(t)
	if !strictEd25519PublicKey(runner.public) {
		t.Fatal("a generated key was rejected")
	}
	x, y, ok := edDecompress(runner.public)
	if !ok || !bytes.Equal(edCompress(x, y), runner.public) {
		t.Fatal("decompression does not round-trip")
	}
	identity := make([]byte, 32)
	identity[0] = 1
	// (0, -1) has order 2; adding it to a real key gives a mixed-order key.
	orderTwo := edCompress(big.NewInt(0), new(big.Int).Sub(edP, edOne))
	mixed := edCompress(new(big.Int).Sub(edP, x), new(big.Int).Sub(edP, y))
	nonCanonical := edCompress(big.NewInt(0), new(big.Int).Add(edP, edOne))
	negativeZero := append([]byte(nil), identity...)
	negativeZero[31] |= 0x80
	for name, key := range map[string][]byte{
		"identity": identity, "order two": orderTwo, "mixed order": mixed,
		"y >= p": nonCanonical, "negative zero": negativeZero,
	} {
		if strictEd25519PublicKey(key) {
			t.Fatalf("%s key accepted", name)
		}
	}
	// A mixed-order signer is refused even though its report would otherwise
	// be well formed.
	report, _ := json.Marshal(CheckAttestation{
		Schema: checkAttestationSchema, Owner: "forkmesh", Repository: "forkmesh",
		Commit: strings.Repeat("a", 40), Check: "CI tests", Conclusion: "success",
		RunID: "r1", IssuedAt: "1", Signer: base64.RawURLEncoding.EncodeToString(mixed),
		Signature: base64.RawURLEncoding.EncodeToString(make([]byte, 64)),
	})
	if _, err := ParseCheckAttestation(report); err == nil || !strings.Contains(err.Error(), "signer") {
		t.Fatalf("mixed-order signer: %v", err)
	}
}

func TestCheckSignerStampsItsOwnClockAndIdentity(t *testing.T) {
	runner := newRunnerIdentity(t)
	commit := strings.Repeat("d", 40)
	var output bytes.Buffer
	input := `{"schema":"forkmesh-check-v1","owner":"ForkMesh","repository":"forkmesh","commit":"` +
		commit + `","check":"CI tests","conclusion":"success","runId":"run-7"}`
	if err := RunCheckSigner(strings.NewReader(input), &output, runner); err != nil {
		t.Fatal(err)
	}
	attestation, err := ParseCheckAttestation(bytes.TrimSpace(output.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if attestation.Owner != "forkmesh" || attestation.Signer != runner.PublicKey() ||
		time.Since(time.UnixMilli(attestation.issuedAtMs)) > time.Minute {
		t.Fatalf("signed attestation: %+v", attestation)
	}
	// Callers cannot backdate or choose the signer.
	for _, extra := range []string{`"issuedAt":1`, `"signer":"x"`} {
		body := strings.Replace(input, `"schema"`, extra+`,"schema"`, 1)
		if err := RunCheckSigner(strings.NewReader(body), &output, runner); err == nil {
			t.Fatalf("signer accepted caller-supplied %s", extra)
		}
	}
	if err := RunCheckSigner(strings.NewReader(strings.Replace(input, "success", "passed", 1)), &output, runner); err == nil {
		t.Fatal("signer accepted an unknown conclusion")
	}
}
