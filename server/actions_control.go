package mirrornode

// Web Actions control requests (www/docs/operations/mirror-actions-control.md).
//
// The gateway is a courier: it writes the routing Worker's signed capability
// next to the exact request body into <mirror-gateway>/actions-control/. This
// file re-verifies that capability against the router public key before the
// runner acts on anything, so the authority for a control request is the
// Worker's owner/writer check plus the router key, never the fact that a file
// appeared in a directory.

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const (
	actionsControlDirectoryName = "actions-control"
	actionsControlEnvelopeType  = "forkmesh.mirror-actions-control"
	actionsControlWireType      = "forkmesh.actions-control-v1"
	maxActionsControlEnvelope   = 16 << 10
	maxActionsControlBody       = 4 << 10
	maxActionsControlFiles      = 64
	maxActionsControlSeen       = 256
	maxActionsControlOutcomes   = 32
	actionsControlMaxAge        = 10 * time.Minute
	actionsControlClockSkew     = time.Minute
	maxSafeJSONInteger          = int64(1)<<53 - 1
)

var (
	actionsControlFilePattern    = regexp.MustCompile(`^[A-Za-z0-9_-]{16,64}\.json$`)
	actionsControlRequestPattern = regexp.MustCompile(`^[A-Za-z0-9_-]{16,64}$`)
	actionsControlActorPattern   = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{0,63}$`)
	actionsControlRefPattern     = regexp.MustCompile(`^refs/heads/[A-Za-z0-9._/-]{1,200}$`)
	actionsCapabilityIDPattern   = regexp.MustCompile(`^[A-Za-z0-9_-]{12,80}$`)
	actionsSignaturePattern      = regexp.MustCompile(`^[A-Za-z0-9_-]{86}$`)
	actionsSegmentPattern        = regexp.MustCompile(`^[A-Za-z0-9._-]{1,100}$`)

	// Extra body keys per action, exactly as the contract lists them.
	actionsControlActions = map[string][]string{
		"dispatch":               {"workflow", "ref"},
		"rerun":                  {"runId"},
		"cancel":                 {"runId"},
		"approve":                {"runId"},
		"reject":                 {"runId"},
		"set-workflow-enabled":   {"workflow", "enabled"},
		"set-repository-enabled": {"enabled"},
	}
	actionsControlCommonKeys = []string{
		"schemaVersion", "type", "requestId", "action", "actor", "issuedAt"}
)

type actionsControlRequest struct {
	RequestID  string
	Action     string
	Actor      string
	Owner      string // from the signed route, never from the body
	Repository string // from the signed route, never from the body
	Workflow   string
	Ref        string
	RunID      int64
	Enabled    bool
	IssuedAt   int64
}

type actionsControlVerdict int

const (
	// Nothing in the file can be trusted: delete it, log locally, tell nobody.
	actionsControlInvalid actionsControlVerdict = iota
	// The capability verified and the body names a request, but the request
	// is stale or malformed. It can be answered as rejected.
	actionsControlRejected
	actionsControlAccepted
)

// actionsCapabilityMessage is byte-identical to edge_routing.request_message
// and tools/mirror_gateway.py request_message.
func actionsCapabilityMessage(node, route, bodySHA256, requestID string, issuedAt int64) []byte {
	return []byte(strings.Join([]string{
		"forkmesh-masked-proxy-v1", node, "POST", route, bodySHA256, requestID,
		strconv.FormatInt(issuedAt, 10),
	}, "\n"))
}

func validControlRef(ref string) bool {
	return actionsControlRefPattern.MatchString(ref) &&
		!strings.Contains(ref, "..") && !strings.Contains(ref, "//") &&
		!strings.HasSuffix(ref, "/") && !strings.HasSuffix(ref, ".lock")
}

func exactJSONKeys(object map[string]json.RawMessage, keys ...string) bool {
	if len(object) != len(keys) {
		return false
	}
	for _, key := range keys {
		if _, ok := object[key]; !ok {
			return false
		}
	}
	return true
}

// jsonInteger accepts a JSON integer literal only: no fraction, no exponent,
// no string, no boolean.
func jsonInteger(raw json.RawMessage, low, high int64) (int64, bool) {
	text := string(bytes.TrimSpace(raw))
	if text == "" || text[0] == '"' || strings.ContainsAny(text, ".eE") {
		return 0, false
	}
	value, err := strconv.ParseInt(text, 10, 64)
	if err != nil || value < low || value > high || strconv.FormatInt(value, 10) != text {
		return 0, false
	}
	return value, true
}

func jsonString(raw json.RawMessage) (string, bool) {
	if len(raw) == 0 || raw[0] != '"' {
		return "", false
	}
	var value string
	if json.Unmarshal(raw, &value) != nil {
		return "", false
	}
	return value, true
}

func jsonBool(raw json.RawMessage) (bool, bool) {
	switch string(bytes.TrimSpace(raw)) {
	case "true":
		return true, true
	case "false":
		return false, true
	}
	return false, false
}

// parseActionsControlEnvelope runs steps 1-3 and 5 of the executor contract
// plus the age half of step 4. The seen-request half belongs to the caller,
// which owns the persisted ring.
func parseActionsControlEnvelope(raw []byte, node, routerPublicKey string, now time.Time) (actionsControlRequest, actionsControlVerdict, error) {
	var request actionsControlRequest
	invalid := func(reason string) (actionsControlRequest, actionsControlVerdict, error) {
		return actionsControlRequest{}, actionsControlInvalid, errors.New(reason)
	}
	if len(raw) == 0 || len(raw) > maxActionsControlEnvelope {
		return invalid("control envelope size is out of bounds")
	}
	routerKey, err := decodeCanonicalBase64(routerPublicKey, ed25519.PublicKeySize)
	if err != nil || !nodePattern.MatchString(node) {
		return invalid("control verification is not configured")
	}

	// Step 1: exact envelope, this node, this repository's route.
	var envelope map[string]json.RawMessage
	if decodeStrictJSON(raw, maxActionsControlEnvelope, &envelope) != nil ||
		!exactJSONKeys(envelope, "schemaVersion", "type", "node", "owner",
			"repository", "receivedAt", "capability", "body") {
		return invalid("control envelope keys are not exact")
	}
	var capability map[string]json.RawMessage
	if decodeStrictJSON(envelope["capability"], maxActionsControlEnvelope, &capability) != nil ||
		!exactJSONKeys(capability, "method", "path", "requestId", "issuedAt",
			"bodySha256", "signature") {
		return invalid("control capability keys are not exact")
	}
	if version, ok := jsonInteger(envelope["schemaVersion"], 1, 1); !ok || version != 1 {
		return invalid("control envelope version is unknown")
	}
	if kind, ok := jsonString(envelope["type"]); !ok || kind != actionsControlEnvelopeType {
		return invalid("control envelope type is unknown")
	}
	if _, ok := jsonInteger(envelope["receivedAt"], 0, maxSafeJSONInteger); !ok {
		return invalid("control envelope receipt time is invalid")
	}
	if named, ok := jsonString(envelope["node"]); !ok || named != node {
		return invalid("control envelope names another node")
	}
	owner, ownerOK := jsonString(envelope["owner"])
	repository, repositoryOK := jsonString(envelope["repository"])
	if !ownerOK || !repositoryOK ||
		!actionsSegmentPattern.MatchString(owner) || !actionsSegmentPattern.MatchString(repository) ||
		owner == "." || owner == ".." || repository == "." || repository == ".." {
		return invalid("control envelope repository is invalid")
	}
	route := "/v1/repositories/" + owner + "/" + repository + "/actions-control"
	method, methodOK := jsonString(capability["method"])
	signedRoute, routeOK := jsonString(capability["path"])
	if !methodOK || !routeOK || method != "POST" || signedRoute != route {
		return invalid("control capability names another route")
	}
	bodySHA256, hashOK := jsonString(capability["bodySha256"])
	capabilityID, idOK := jsonString(capability["requestId"])
	signatureText, signatureOK := jsonString(capability["signature"])
	issuedAt, issuedOK := jsonInteger(capability["issuedAt"], 1, maxSafeJSONInteger)
	if !hashOK || !idOK || !signatureOK || !issuedOK ||
		!digestPattern.MatchString(bodySHA256) ||
		!actionsCapabilityIDPattern.MatchString(capabilityID) ||
		!actionsSignaturePattern.MatchString(signatureText) {
		return invalid("control capability is malformed")
	}

	// Step 2: the capability is bound to these exact body bytes.
	bodyText, bodyOK := jsonString(envelope["body"])
	if !bodyOK || bodyText == "" || len(bodyText) > maxActionsControlBody {
		return invalid("control body size is out of bounds")
	}
	sum := sha256.Sum256([]byte(bodyText))
	if hex.EncodeToString(sum[:]) != bodySHA256 {
		return invalid("control body hash does not match its capability")
	}

	// Step 3: the routing Worker signed exactly this request for this node.
	signature, err := decodeCanonicalBase64(signatureText, ed25519.SignatureSize)
	if err != nil || !strictEd25519PublicKey(routerKey) ||
		!ed25519.Verify(ed25519.PublicKey(routerKey),
			actionsCapabilityMessage(node, route, bodySHA256, capabilityID, issuedAt), signature) {
		return invalid("control capability signature is invalid")
	}

	// The body is now authentic. From here a failure can be reported back to
	// the browser, provided the body says which request it was.
	var body map[string]json.RawMessage
	if decodeStrictJSON([]byte(bodyText), maxActionsControlBody, &body) != nil {
		return invalid("verified control body is not a JSON object")
	}
	requestID, requestOK := jsonString(body["requestId"])
	action, actionOK := jsonString(body["action"])
	extra, known := actionsControlActions[action]
	if !requestOK || !actionOK || !known || !actionsControlRequestPattern.MatchString(requestID) {
		return invalid("verified control body names no request or action")
	}
	request = actionsControlRequest{
		RequestID: requestID, Action: action, Owner: owner, Repository: repository}
	rejected := func(reason string) (actionsControlRequest, actionsControlVerdict, error) {
		return request, actionsControlRejected, errors.New(reason)
	}

	// Step 4, age half.
	issued := time.UnixMilli(issuedAt)
	if now.Sub(issued) > actionsControlMaxAge || issued.Sub(now) > actionsControlClockSkew {
		return rejected("control capability is stale")
	}

	// Step 5: the same exact-key rules as the gateway.
	if !exactJSONKeys(body, append(append([]string(nil), actionsControlCommonKeys...), extra...)...) {
		return rejected("control body keys are not exact")
	}
	if version, ok := jsonInteger(body["schemaVersion"], 1, 1); !ok || version != 1 {
		return rejected("control body version is unknown")
	}
	if kind, ok := jsonString(body["type"]); !ok || kind != actionsControlWireType {
		return rejected("control body type is unknown")
	}
	actor, actorOK := jsonString(body["actor"])
	bodyIssuedAt, bodyIssuedOK := jsonInteger(body["issuedAt"], 1, maxSafeJSONInteger)
	if !actorOK || !bodyIssuedOK || !actionsControlActorPattern.MatchString(actor) {
		return rejected("control body actor or time is invalid")
	}
	request.Actor = actor
	request.IssuedAt = bodyIssuedAt
	for _, key := range extra {
		ok := false
		switch key {
		case "workflow":
			request.Workflow, ok = jsonString(body[key])
			ok = ok && workflowPathPattern.MatchString(request.Workflow)
		case "ref":
			request.Ref, ok = jsonString(body[key])
			ok = ok && validControlRef(request.Ref)
		case "runId":
			request.RunID, ok = jsonInteger(body[key], 1, maxSafeJSONInteger)
		case "enabled":
			request.Enabled, ok = jsonBool(body[key])
		}
		if !ok {
			return rejected("control body field " + key + " is invalid")
		}
	}
	return request, actionsControlAccepted, nil
}

type actionsControlFile struct {
	Name    string
	Bytes   []byte
	Problem string
}

// scanActionsControlDirectory returns the regular, single-link, non-symlink
// <requestId>.json files of at most 16 KiB in a real directory named
// actions-control. Other names, including the gateway's in-flight temporary
// files, are ignored.
func scanActionsControlDirectory(directory string, maximum int) []actionsControlFile {
	if filepath.Base(directory) != actionsControlDirectoryName {
		return nil
	}
	info, err := os.Lstat(directory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return nil
	}
	entries, err := os.ReadDir(directory)
	if err != nil {
		return nil
	}
	var names []string
	for _, entry := range entries {
		if actionsControlFilePattern.MatchString(entry.Name()) {
			names = append(names, entry.Name())
		}
	}
	sort.Strings(names)
	if len(names) > maximum {
		names = names[:maximum]
	}
	var files []actionsControlFile
	for _, name := range names {
		file := actionsControlFile{Name: name}
		handle, err := os.OpenFile(filepath.Join(directory, name),
			os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
		if err != nil {
			file.Problem = "control file cannot be opened without following a link"
			files = append(files, file)
			continue
		}
		stat, err := handle.Stat()
		var links uint64 = 1
		if raw, ok := stat.Sys().(*syscall.Stat_t); err == nil && ok {
			links = uint64(raw.Nlink)
		}
		switch {
		case err != nil || !stat.Mode().IsRegular():
			file.Problem = "control file is not a regular file"
		case links != 1:
			file.Problem = "control file has more than one link"
		case stat.Size() <= 0 || stat.Size() > maxActionsControlEnvelope:
			file.Problem = "control file size is out of bounds"
		default:
			data := make([]byte, maxActionsControlEnvelope+1)
			count, _ := handle.Read(data)
			if count <= 0 || count > maxActionsControlEnvelope {
				file.Problem = "control file size is out of bounds"
			} else {
				file.Bytes = data[:count]
			}
		}
		handle.Close()
		files = append(files, file)
	}
	return files
}

// removeActionsControlFile unlinks one scanned entry. Unlink never follows a
// symbolic link, and the name was matched against the request-id pattern.
func removeActionsControlFile(directory, name string) {
	if actionsControlFilePattern.MatchString(name) {
		_ = os.Remove(filepath.Join(directory, name))
	}
}

func rememberActionsControl(ring []string, requestID string) []string {
	ring = append(ring, requestID)
	if len(ring) > maxActionsControlSeen {
		ring = ring[len(ring)-maxActionsControlSeen:]
	}
	return ring
}

func actionsControlSeen(ring []string, requestID string) bool {
	for _, seen := range ring {
		if seen == requestID {
			return true
		}
	}
	return false
}
