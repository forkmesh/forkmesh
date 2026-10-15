package update

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

func jsonString(value any) string {
	text, _ := value.(string)
	return text
}

func withPath(base *url.URL, path string, keepQuery bool) *url.URL {
	out := *base
	out.Path = path
	out.RawPath = ""
	if !keepQuery {
		out.RawQuery = ""
		out.ForceQuery = false
	}
	return &out
}

func parseBaseURL(base string) *url.URL {
	parsed, err := url.Parse(strings.TrimSpace(base))
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return nil
	}
	return parsed
}

func resolveInstallCloneURL(ctx context.Context, client *http.Client, base string, timeout time.Duration) string {
	parsed := parseBaseURL(base)
	if parsed == nil {
		return ""
	}
	if client == nil {
		client = http.DefaultClient
	}
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, withPath(parsed, "/api/install-source", false).String(), nil)
	if err != nil {
		return ""
	}
	response, err := client.Do(request)
	if err != nil {
		return ""
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	var object map[string]any
	if json.Unmarshal(body, &object) != nil {
		return ""
	}
	node := strings.TrimSpace(jsonString(object["node"]))
	repo := "forkmesh"
	if text, ok := object["repo"].(string); ok {
		repo = text
	}
	repo = strings.TrimSpace(repo)
	if node == "" || repo == "" {
		return ""
	}
	clone := withPath(parsed, "/forkmesh/"+repo, false)
	clone.Fragment = ""
	clone.RawFragment = ""
	return clone.String()
}

func (j *job) findMirror() string {
	j.out.phase("finding a mirror", 0)
	j.out.percent(5)
	if j.req.AsUser == "" {
		j.out.status("Finding an online ForkMesh mirror...", false)
	} else {
		j.out.status("Finding an online ForkMesh mirror (installing for "+j.req.AsUser+")...", false)
	}
	return resolveInstallCloneURL(j.ctx, j.env.HTTP, j.req.CatalogURL, j.env.Lookup)
}
