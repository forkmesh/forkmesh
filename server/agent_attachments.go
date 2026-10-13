package mirrornode

// Images attached to a website-queued agent prompt.
//
// The claim carries only each image's metadata. Before the provider CLI runs,
// the node downloads every image over the same signed request plus the job's
// current lease, checks its size, digest and magic bytes, and writes it into
// the job's working tree as .forkmesh/attachments/<uuid>.<ext>, a name this
// node picks (the client's file name is never used for a path). The folder is
// listed in the clone's .git/info/exclude so the images are never committed,
// and the prompt gains a block naming each file. A job whose images cannot be
// fetched or written fails rather than running without them.

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
)

const (
	agentAttachmentMaxCount = 5
	agentAttachmentMaxBytes = 5 << 20
	// Relative to the job's working tree; the same text the phone's on-device
	// runs use.
	agentAttachmentDir         = ".forkmesh/attachments"
	agentAttachmentPromptTitle = "Attached images (open them with your file-reading tool):"
)

var (
	agentAttachmentIDPattern     = regexp.MustCompile(`^[0-9a-f]{32}$`)
	agentAttachmentDigestPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)
	agentAttachmentExtensions    = map[string]string{
		"image/jpeg": "jpg", "image/png": "png", "image/webp": "webp", "image/gif": "gif",
	}
)

type agentAttachment struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	MediaType string `json:"mediaType"`
	Size      int64  `json:"size"`
	Sha256    string `json:"sha256"`
}

// validAgentAttachments checks the claim's metadata before anything runs.
func validAgentAttachments(attachments []agentAttachment) bool {
	if len(attachments) > agentAttachmentMaxCount {
		return false
	}
	for _, a := range attachments {
		if !agentAttachmentIDPattern.MatchString(a.ID) ||
			agentAttachmentExtensions[a.MediaType] == "" ||
			a.Size <= 0 || a.Size > agentAttachmentMaxBytes ||
			!agentAttachmentDigestPattern.MatchString(a.Sha256) {
			return false
		}
	}
	return true
}

func agentAttachmentMagicOK(mediaType string, data []byte) bool {
	switch mediaType {
	case "image/jpeg":
		return bytes.HasPrefix(data, []byte{0xff, 0xd8, 0xff})
	case "image/png":
		return bytes.HasPrefix(data, []byte("\x89PNG\r\n\x1a\n"))
	case "image/gif":
		return bytes.HasPrefix(data, []byte("GIF87a")) || bytes.HasPrefix(data, []byte("GIF89a"))
	case "image/webp":
		return len(data) >= 12 && bytes.HasPrefix(data, []byte("RIFF")) && string(data[8:12]) == "WEBP"
	}
	return false
}

func newAgentAttachmentUUID() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	raw[6] = raw[6]&0x0f | 0x40
	raw[8] = raw[8]&0x3f | 0x80
	h := hex.EncodeToString(raw[:])
	return h[0:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20] + "-" + h[20:32], nil
}

// agentAttachmentPromptBlock is appended to the prompt: a blank line, the
// header, then one "- <path>" line per image.
func agentAttachmentPromptBlock(paths []string) string {
	if len(paths) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString("\n\n")
	b.WriteString(agentAttachmentPromptTitle)
	for _, path := range paths {
		b.WriteString("\n- ")
		b.WriteString(path)
	}
	return b.String()
}

// excludeAgentAttachments lists the attachment folder in the clone's
// .git/info/exclude, once.
func excludeAgentAttachments(ctx context.Context, workdir string) error {
	path, err := agentGit(ctx, workdir, "rev-parse", "--git-path", "info/exclude")
	if err != nil || path == "" {
		path = filepath.Join(".git", "info", "exclude")
	}
	if !filepath.IsAbs(path) {
		path = filepath.Join(workdir, path)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	return appendAgentAttachmentExclude(path)
}

func appendAgentAttachmentExclude(path string) error {
	if info, err := os.Lstat(path); err == nil && !info.Mode().IsRegular() {
		return fmt.Errorf("%s is not a plain file", path)
	}
	existing, err := os.ReadFile(path)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	line := "/" + agentAttachmentDir + "/"
	for _, current := range strings.Split(string(existing), "\n") {
		if strings.TrimSpace(current) == line {
			return nil
		}
	}
	file, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY|syscall.O_NOFOLLOW, 0600)
	if err != nil {
		return err
	}
	prefix := ""
	if len(existing) > 0 && !bytes.HasSuffix(existing, []byte("\n")) {
		prefix = "\n"
	}
	_, err = file.WriteString(prefix + line + "\n")
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	return err
}

func plainAgentFolder(root, rel string, own func(string) error) (string, error) {
	dir := root
	for _, part := range strings.Split(rel, "/") {
		dir = filepath.Join(dir, part)
		info, err := os.Lstat(dir)
		switch {
		case errors.Is(err, os.ErrNotExist):
			if err := os.Mkdir(dir, 0700); err != nil {
				return "", err
			}
		case err != nil:
			return "", err
		case !info.IsDir():
			return "", fmt.Errorf("%s is not a plain folder", filepath.ToSlash(rel))
		}
		if own != nil {
			if err := own(dir); err != nil {
				return "", err
			}
		}
	}
	return dir, nil
}

// materializeAttachments downloads the job's images into workdir and returns
// their workdir-relative paths, in the job's order.
func (r *agentJobRunner) materializeAttachments(ctx context.Context, work agentWork, workdir string) ([]string, error) {
	return writeAgentAttachments(work.job.Attachments, workdir,
		func() error { return excludeAgentAttachments(ctx, workdir) },
		func(attachment agentAttachment) ([]byte, error) { return r.fetchAttachment(ctx, work, attachment) },
		nil)
}

func writeAgentAttachments(attachments []agentAttachment, workdir string, exclude func() error,
	fetch func(agentAttachment) ([]byte, error), own func(string) error) ([]string, error) {
	if len(attachments) == 0 {
		return nil, nil
	}
	if !validAgentAttachments(attachments) {
		return nil, errors.New("the job's image attachments are malformed")
	}
	if err := exclude(); err != nil {
		return nil, fmt.Errorf("exclude the attachment folder: %w", err)
	}
	dir, err := plainAgentFolder(workdir, agentAttachmentDir, own)
	if err != nil {
		return nil, err
	}
	paths := make([]string, 0, len(attachments))
	for i, attachment := range attachments {
		data, err := fetch(attachment)
		if err != nil {
			return nil, fmt.Errorf("image %d of %d: %w", i+1, len(attachments), err)
		}
		id, err := newAgentAttachmentUUID()
		if err != nil {
			return nil, err
		}
		name := id + "." + agentAttachmentExtensions[attachment.MediaType]
		if err := writeNewAgentFile(filepath.Join(dir, name), data); err != nil {
			return nil, err
		}
		if own != nil {
			if err := own(filepath.Join(dir, name)); err != nil {
				return nil, err
			}
		}
		paths = append(paths, agentAttachmentDir+"/"+name)
	}
	return paths, nil
}

func writeNewAgentFile(path string, data []byte) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, err = file.Write(data)
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	return err
}

func (r *agentJobRunner) currentLease(job agentJob) string {
	r.mu.Lock()
	defer r.mu.Unlock()
	if lease := r.leases[job.JobID]; lease != "" {
		return lease
	}
	return job.LeaseID
}

func (r *agentJobRunner) fetchAttachment(ctx context.Context, work agentWork, attachment agentAttachment) ([]byte, error) {
	endpoint, err := r.daemon.collaborationURL(
		Repository{Owner: r.node(), Name: work.repo.Name},
		"org-agent-jobs/"+strconv.FormatInt(work.job.JobID, 10)+"/attachments/"+attachment.ID)
	if err != nil {
		return nil, err
	}
	query := r.daemon.signedCollaborationQuery()
	query.Set("leaseId", r.currentLease(work.job))
	endpoint.RawQuery = query.Encode()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return nil, err
	}
	request.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+r.daemon.config.Version)
	response, err := agentJobHTTPClient.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, attachment.Size+1))
	if err != nil {
		return nil, err
	}
	return checkAgentAttachment(attachment, response.StatusCode, data)
}

func checkAgentAttachment(attachment agentAttachment, status int, data []byte) ([]byte, error) {
	if status < 200 || status >= 300 {
		return nil, fmt.Errorf("the relay returned HTTP %d: %s",
			status, boundedText(data, 240))
	}
	digest := sha256.Sum256(data)
	switch {
	case int64(len(data)) != attachment.Size:
		return nil, errors.New("the downloaded image has the wrong size")
	case hex.EncodeToString(digest[:]) != attachment.Sha256:
		return nil, errors.New("the downloaded image does not match its digest")
	case !agentAttachmentMagicOK(attachment.MediaType, data):
		return nil, errors.New("the downloaded file is not the image type it claims to be")
	}
	return data, nil
}
