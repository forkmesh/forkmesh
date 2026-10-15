package update

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"time"
)

type Mirror struct {
	Identity string `json:"identity"`
	CasRoot  string `json:"casRoot"`
}

type PrebuiltRequest struct {
	Request
	Tag       string   `json:"tag"`
	TagCommit string   `json:"tagCommit"`
	OS        string   `json:"os"`
	Arch      string   `json:"arch"`
	Mirrors   []Mirror `json:"mirrors"`
	TempDir   string   `json:"tempDir"`
}

type prebuiltPlan struct {
	tag           string
	owner         string
	name          string
	assetHash     string
	assetName     string
	companionHash string
	localBlob     string
	tempDir       string
	catalogURL    string
	mirrors       []Mirror
}

var sha256Pattern = regexp.MustCompile(`\A[0-9a-f]{64}\z`)

const manifestPath = ":.forkmesh/releases/latest/release.json"

func currentOS() string {
	switch runtime.GOOS {
	case "linux":
		return "linux"
	case "darwin":
		return "macos"
	case "windows":
		return "windows"
	}
	return ""
}

func currentArch() string {
	switch runtime.GOARCH {
	case "amd64":
		return "x86_64"
	case "386":
		return "i386"
	case "ppc64", "ppc64le":
		return "power64"
	case "mips64", "mips64le":
		return "mips64"
	case "mips", "mipsle":
		return "mips"
	case "loong64":
		return "loongarch64"
	}
	return runtime.GOARCH
}

func gitCapture(ctx context.Context, dir string, args ...string) ([]byte, bool) {
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "git", args...)
	cmd.Dir = dir
	cmd.WaitDelay = 2 * time.Second
	out, err := cmd.Output()
	return out, err == nil
}

func blobPath(casRoot, hash string) string {
	return filepath.Join(casRoot, hash[:2], hash, "data")
}

func fileSha256Matches(path, expected string) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	defer file.Close()
	hasher := sha256.New()
	if _, err := io.Copy(hasher, file); err != nil {
		return false
	}
	return hex.EncodeToString(hasher.Sum(nil)) == strings.ToLower(expected)
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func percentEncode(value string) string {
	var out strings.Builder
	for _, b := range []byte(value) {
		switch {
		case b >= 'A' && b <= 'Z', b >= 'a' && b <= 'z', b >= '0' && b <= '9', b == '-', b == '.', b == '_', b == '~':
			out.WriteByte(b)
		default:
			fmt.Fprintf(&out, "%%%02X", b)
		}
	}
	return out.String()
}

func releaseBlobURL(catalogURL, owner, name, hash string) string {
	base := parseBaseURL(catalogURL)
	if base == nil {
		return ""
	}
	out := withPath(base, "/api/repo/"+owner+"/"+name+"/releases/blob/sha256/"+hash, true)
	out.RawPath = "/api/repo/" + percentEncode(owner) + "/" + percentEncode(name) + "/releases/blob/sha256/" + hash
	return out.String()
}

func objectArray(value any) []map[string]any {
	items, _ := value.([]any)
	var out []map[string]any
	for _, item := range items {
		object, _ := item.(map[string]any)
		if object == nil {
			object = map[string]any{}
		}
		out = append(out, object)
	}
	return out
}

func (m *Module) systemLog(text string) { m.logSystem(text) }

func (m *Module) logSystem(text string) {
	if m.LogSystem != nil {
		m.LogSystem(text)
		return
	}
	if m.core != nil {
		m.core.LogUI("%s", text)
	}
}

func (m *Module) planPrebuilt(ctx context.Context, p PrebuiltRequest) (*prebuiltPlan, bool) {
	tag := p.Tag
	wanted := strings.ToLower(strings.TrimSpace(p.TagCommit))
	wantOS := p.OS
	if wantOS == "" {
		wantOS = currentOS()
	}
	wantArch := p.Arch
	if wantArch == "" {
		wantArch = currentArch()
	}
	if wantOS == "" {
		return nil, false
	}
	publishes := false
	manifestFor := func(revision string) map[string]any {
		out, ok := gitCapture(ctx, p.ClientDir, "show", revision+manifestPath)
		if !ok {
			return nil
		}
		publishes = true
		var candidate map[string]any
		if json.Unmarshal(out, &candidate) != nil || candidate == nil {
			return nil
		}
		if strings.TrimSpace(jsonString(candidate["tag"])) != tag {
			return nil
		}
		released := strings.ToLower(strings.TrimSpace(jsonString(candidate["tag_commit"])))
		if released == "" {
			return nil
		}
		if released != wanted {
			if _, ok := gitCapture(ctx, p.ClientDir, "merge-base", "--is-ancestor", released, wanted); !ok {
				return nil
			}
		}
		return candidate
	}
	manifest := manifestFor(wanted)
	if manifest == nil {
		manifest = manifestFor("@{upstream}")
	}
	if manifest == nil {
		if publishes {
			m.mu.Lock()
			first := m.waitingLoggedFor != tag
			m.waitingLoggedFor = tag
			m.mu.Unlock()
			if first {
				m.logSystem(fmt.Sprintf("Auto-update: release %s is tagged but its signed artifacts are not published yet; waiting.", tag))
			}
			return nil, true
		}
		m.logSystem(fmt.Sprintf("Auto-update: release %s publishes no artifact manifest; building from source.", tag))
		return nil, false
	}
	plan := &prebuiltPlan{tag: tag, mirrors: p.Mirrors, catalogURL: p.CatalogURL, tempDir: p.TempDir}
	if plan.tempDir == "" {
		plan.tempDir = os.TempDir()
	}
	for _, asset := range objectArray(manifest["assets"]) {
		if strings.TrimSpace(jsonString(asset["os"])) != wantOS {
			continue
		}
		if strings.TrimSpace(jsonString(asset["arch"])) != wantArch {
			continue
		}
		hash := strings.ToLower(strings.TrimSpace(jsonString(asset["blob_sha256"])))
		if !sha256Pattern.MatchString(hash) {
			continue
		}
		name := jsonString(asset["name"])
		if strings.HasPrefix(name, "forkmesh-mirror-node") {
			if plan.companionHash == "" {
				plan.companionHash = hash
			}
			continue
		}
		if plan.assetHash == "" {
			plan.assetHash = hash
			plan.assetName = name
		}
	}
	manifestRepo := strings.TrimSpace(jsonString(manifest["repo"]))
	slash := strings.Index(manifestRepo, "/")
	if plan.assetHash == "" || slash <= 0 {
		m.logSystem(fmt.Sprintf("Auto-update: release %s has no prebuilt artifact for %s/%s; building from source.", tag, wantOS, wantArch))
		return nil, false
	}
	plan.owner = manifestRepo[:slash]
	plan.name = manifestRepo[slash+1:]
	for _, mirror := range p.Mirrors {
		if strings.TrimSpace(mirror.CasRoot) == "" || !strings.EqualFold(mirror.Identity, manifestRepo) {
			continue
		}
		blob := blobPath(mirror.CasRoot, plan.assetHash)
		if fileExists(blob) && fileSha256Matches(blob, plan.assetHash) {
			m.logSystem(fmt.Sprintf("Auto-update: installing release %s from this node's own artifact store (%s).", tag, plan.assetName))
			plan.localBlob = blob
			return plan, true
		}
	}
	part, err := os.OpenFile(plan.stagingPath()+".part", os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return nil, false
	}
	part.Close()
	return plan, true
}

func (p *prebuiltPlan) stagingPath() string {
	return filepath.Join(p.tempDir, "forkmesh-update-"+p.assetHash[:12])
}

func (j *job) download(target *os.File, address, expected string) (bool, string) {
	defer target.Close()
	hasher := sha256.New()
	if address == "" {
		return false, "the catalog address is not a URL"
	}
	request, err := http.NewRequestWithContext(j.ctx, http.MethodGet, address, nil)
	if err != nil {
		return false, err.Error()
	}
	client := j.env.HTTP
	if client == nil {
		client = http.DefaultClient
	}
	response, err := client.Do(request)
	if err != nil {
		return false, err.Error()
	}
	defer response.Body.Close()
	ok, reason := true, ""
	if response.StatusCode >= 400 {
		ok = false
		reason = fmt.Sprintf("Error transferring %s - server replied: %s", address, http.StatusText(response.StatusCode))
	}
	if _, err := io.Copy(io.MultiWriter(target, hasher), response.Body); err != nil && ok {
		ok, reason = false, err.Error()
	}
	if err := target.Close(); err != nil && ok {
		ok, reason = false, err.Error()
	}
	if !ok {
		return false, reason
	}
	if hex.EncodeToString(hasher.Sum(nil)) != expected {
		return false, "checksum mismatch"
	}
	return true, ""
}

func (j *job) downloadArtifact() string {
	p := j.prebuilt
	staging := p.stagingPath()
	part := staging + ".part"
	file, err := os.OpenFile(part, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return ""
	}
	j.out.systemLog(fmt.Sprintf("Auto-update: downloading the release %s artifact (%s)...", p.tag, p.assetName))
	ok, reason := j.download(file, releaseBlobURL(p.catalogURL, p.owner, p.name, p.assetHash), p.assetHash)
	if !ok {
		_ = os.Remove(part)
		if j.cancelled() {
			return ""
		}
		j.out.systemLog(fmt.Sprintf("Auto-update: artifact download for %s failed (%s); falling back to a source build.", p.tag, reason))
		return ""
	}
	_ = os.Remove(staging)
	if os.Rename(part, staging) != nil {
		_ = os.Remove(part)
		return ""
	}
	return staging
}

func (j *job) refreshCompanion() {
	p := j.prebuilt
	binDir := absoluteDir(j.req.AppPath)
	installed := filepath.Join(binDir, "forkmesh-mirror-node")
	if p.companionHash == "" {
		j.out.systemLog(fmt.Sprintf("Auto-update: release %s publishes no Go mirror-node companion; keeping the current one.", p.tag))
		return
	}
	if fileSha256Matches(installed, p.companionHash) {
		return
	}
	for _, mirror := range p.mirrors {
		if strings.TrimSpace(mirror.CasRoot) == "" {
			continue
		}
		blob := blobPath(mirror.CasRoot, p.companionHash)
		if !fileExists(blob) || !fileSha256Matches(blob, p.companionHash) {
			continue
		}
		if installMirrorNodeCompanionFile(blob, binDir) {
			j.out.systemLog(fmt.Sprintf("Auto-update: installed the release %s Go mirror-node companion from this node's own artifact store.", p.tag))
		} else {
			j.out.systemLog(fmt.Sprintf("Auto-update: could not refresh the Go mirror-node companion for %s; continuing the client update.", p.tag))
		}
		return
	}
	part := filepath.Join(p.tempDir, "forkmesh-mirror-node-update-"+p.companionHash[:12]+".part")
	file, err := os.OpenFile(part, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	j.out.systemLog(fmt.Sprintf("Auto-update: downloading the release %s Go mirror-node companion...", p.tag))
	ok, _ := j.download(file, releaseBlobURL(p.catalogURL, p.owner, p.name, p.companionHash), p.companionHash)
	if j.cancelled() {
		_ = os.Remove(part)
		return
	}
	if ok && installMirrorNodeCompanionFile(part, binDir) {
		j.out.systemLog(fmt.Sprintf("Auto-update: installed the release %s Go mirror-node companion.", p.tag))
	} else {
		j.out.systemLog(fmt.Sprintf("Auto-update: could not refresh the Go mirror-node companion for %s; continuing the client update.", p.tag))
	}
	_ = os.Remove(part)
}

func (j *job) runPrebuilt() outcome {
	p := j.prebuilt
	artifact := p.localBlob
	if artifact == "" {
		artifact = j.downloadArtifact()
		if j.cancelled() {
			return outcomeCancelled
		}
		if artifact == "" {
			return outcomeFallback
		}
	}
	j.refreshCompanion()
	if j.cancelled() {
		return outcomeCancelled
	}
	staged := filepath.Join(p.tempDir, "forkmesh-update-staged")
	if staged != artifact {
		_ = os.Remove(staged)
		if copyFile(artifact, staged) != nil {
			j.out.systemLog(fmt.Sprintf("Auto-update: could not stage the %s artifact; falling back to a source build.", p.tag))
			return outcomeFallback
		}
	}
	_ = os.Chmod(staged, 0o755)
	j.out.restartLog(fmt.Sprintf("prebuilt update to %s staged", p.tag))
	j.req.AsUser = ""
	j.built = staged
	return j.install()
}
