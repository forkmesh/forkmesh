package hosts

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	cloudflareAPIBase = "https://api.cloudflare.com/client/v4"
	apiCallTimeout    = 120 * time.Second
	apiBodyLimit      = 1 << 20
)

func storedCredential(variables map[string]string, names []string) string {
	keys := make([]string, 0, len(variables))
	for key := range variables {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, name := range names {
		for _, key := range keys {
			if !strings.EqualFold(strings.TrimSpace(key), name) {
				continue
			}
			value := strings.TrimSpace(variables[key])
			if value == "" || strings.ContainsAny(value, "\x00\n\r") {
				continue
			}
			return value
		}
	}
	return ""
}

var vultrKeyVariableNames = []string{"VULTR_API_KEY", "VULTR_API_TOKEN", "VULTR_TOKEN", "VULTR_KEY"}

func cloudflareAPITokenFromVariables(variables map[string]string) string {
	return storedCredential(variables, []string{"CLOUDFLARE_API_TOKEN", "CF_API_TOKEN", "CLOUDFLARE_TOKEN", "CF_TOKEN"})
}

func cloudflareZoneNameFromVariables(variables map[string]string) string {
	return storedCredential(variables, []string{"CLOUDFLARE_ZONE", "CLOUDFLARE_ZONE_NAME", "CF_ZONE"})
}

func vultrAPIKeyFromVariables(variables map[string]string) string {
	return storedCredential(variables, vultrKeyVariableNames)
}

func (m *Module) vultrAPIKey(formKey string) (string, bool) {
	if key := strings.TrimSpace(formKey); key != "" {
		return key, false
	}
	key := vultrAPIKeyFromVariables(m.actionVariables())
	return key, key != ""
}

var (
	dnsLabelPattern = regexp.MustCompile(`^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$`)
	dnsZonePattern  = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$`)
)

func vultrMirrorDNSHostname(nodeName, zoneName string) string {
	node := strings.ToLower(strings.TrimSpace(nodeName))
	zone := strings.ToLower(strings.TrimSpace(zoneName))
	for strings.HasSuffix(zone, ".") {
		zone = zone[:len(zone)-1]
	}
	if !dnsLabelPattern.MatchString(node) || !dnsZonePattern.MatchString(zone) {
		return ""
	}
	hostname := node + "." + zone
	if len(hostname) > 253 {
		return ""
	}
	return hostname
}

func (m *Module) cloudflareBase() string {
	if m.CloudflareBase != "" {
		return m.CloudflareBase
	}
	return cloudflareAPIBase
}

func transferError(response *http.Response, err error, url string) string {
	if err != nil {
		return err.Error()
	}
	return "Error transferring " + url + " - server replied: " + strings.TrimSpace(strings.TrimPrefix(response.Status, strconv.Itoa(response.StatusCode)))
}

func (m *Module) apiRequest(ctx context.Context, method, url, token string, body []byte) (int, map[string]any, string) {
	ctx, cancel := context.WithTimeout(ctx, apiCallTimeout)
	defer cancel()
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	request, err := http.NewRequestWithContext(ctx, method, url, reader)
	if err != nil {
		return 0, map[string]any{}, err.Error()
	}
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("Content-Type", "application/json")
	response, err := m.httpClient().Do(request)
	if err != nil {
		return 0, map[string]any{}, transferError(nil, err, url)
	}
	defer response.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(response.Body, apiBodyLimit))
	object := map[string]any{}
	_ = json.Unmarshal(raw, &object)
	if object == nil {
		object = map[string]any{}
	}
	failure := ""
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		failure = transferError(response, nil, url)
	}
	return response.StatusCode, object, failure
}

func (m *Module) vultrCall(ctx context.Context, apiKey, method, path string, body map[string]any) (map[string]any, string) {
	var payload []byte
	switch {
	case method == http.MethodPost:
		if body == nil {
			body = map[string]any{}
		}
		payload = []byte(compactJSON(body))
	case method == http.MethodGet || method == "":
		method = http.MethodGet
	default:
		if len(body) > 0 {
			payload = []byte(compactJSON(body))
		}
	}
	status, object, failure := m.apiRequest(ctx, method, m.vultrBase()+path, apiKey, payload)
	if failure != "" || status < 200 || status >= 300 {
		detail := jsonString(object["error"])
		if detail == "" {
			detail = failure
		}
		return map[string]any{}, "Vultr API error (HTTP " + strconv.Itoa(status) + "): " + detail
	}
	m.rememberVultrKey(ctx, apiKey)
	return object, ""
}

func (m *Module) cloudflareCall(ctx context.Context, token, method, path string, body map[string]any) (map[string]any, string) {
	var payload []byte
	if method != http.MethodGet {
		if body == nil {
			body = map[string]any{}
		}
		payload = []byte(compactJSON(body))
	}
	status, object, failure := m.apiRequest(ctx, method, m.cloudflareBase()+path, token, payload)
	succeeded, _ := object["success"].(bool)
	if failure != "" || status < 200 || status >= 300 || !succeeded {
		detail := ""
		if errorsList, ok := object["errors"].([]any); ok && len(errorsList) > 0 {
			if first, ok := errorsList[0].(map[string]any); ok {
				detail = jsonString(first["message"])
			}
		}
		if detail == "" {
			detail = failure
		}
		if detail == "" {
			detail = "Unknown error"
		}
		return map[string]any{}, "Cloudflare API error (HTTP " + strconv.Itoa(status) + "): " + detail
	}
	return object, ""
}

func (m *Module) postJSON(ctx context.Context, url, bearer string, body map[string]any) (int, map[string]any, string) {
	ctx, cancel := context.WithTimeout(ctx, apiCallTimeout)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader([]byte(compactJSON(body))))
	if err != nil {
		return 0, map[string]any{}, err.Error()
	}
	request.Header.Set("Content-Type", "application/json")
	if bearer != "" {
		request.Header.Set("Authorization", "Bearer "+bearer)
	}
	response, err := m.httpClient().Do(request)
	if err != nil {
		return 0, map[string]any{}, err.Error()
	}
	defer response.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(response.Body, apiBodyLimit))
	object := map[string]any{}
	_ = json.Unmarshal(raw, &object)
	if object == nil {
		object = map[string]any{}
	}
	failure := ""
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		failure = transferError(response, nil, url)
	}
	return response.StatusCode, object, failure
}
