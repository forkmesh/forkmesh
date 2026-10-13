package mirrornode

// VultrPool is the PoolProvider for Vultr (runnerpool.go). It only ever lists,
// creates or destroys instances labelled fmnext-burst-* and tagged
// forkmesh-next, and re-reads an instance before deleting it so a stale or
// forged id cannot reach anything else on the account. The key should be a
// Vultr sub-user limited to the instances permission.

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"time"
)

const vultrAPI = "https://api.vultr.com/v2"

var vultrIDPattern = regexp.MustCompile(`^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$`)

type VultrPool struct {
	APIKey     string
	Region     string
	Plan       string
	SnapshotID string
	// UserData is the optional cloud-init that starts the runner daemon.
	UserData string
	// BaseURL overrides the API for tests; empty means api.vultr.com.
	BaseURL string
	Client  *http.Client
}

type vultrInstance struct {
	ID          string   `json:"id"`
	Label       string   `json:"label"`
	Tags        []string `json:"tags"`
	DateCreated string   `json:"date_created"`
}

func (v *VultrPool) instance(raw vultrInstance) PoolInstance {
	created, _ := time.Parse(time.RFC3339, raw.DateCreated)
	return PoolInstance{ID: raw.ID, Label: raw.Label, Tags: raw.Tags, CreatedAt: created}
}

func (v *VultrPool) do(ctx context.Context, method, path string, body any, out any) error {
	base := v.BaseURL
	if base == "" {
		base = vultrAPI
	}
	var reader io.Reader
	if body != nil {
		raw, err := json.Marshal(body)
		if err != nil {
			return err
		}
		reader = bytes.NewReader(raw)
	}
	request, err := http.NewRequestWithContext(ctx, method, base+path, reader)
	if err != nil {
		return err
	}
	request.Header.Set("Authorization", "Bearer "+v.APIKey)
	request.Header.Set("Content-Type", "application/json")
	client := v.Client
	if client == nil {
		client = &http.Client{Timeout: 30 * time.Second}
	}
	response, err := client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	reply, _ := io.ReadAll(io.LimitReader(response.Body, 4<<20))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("vultr %s %s: HTTP %d: %s", method, path, response.StatusCode, boundedText(reply, 200))
	}
	if out != nil && len(reply) != 0 {
		return json.Unmarshal(reply, out)
	}
	return nil
}

func (v *VultrPool) List(ctx context.Context) ([]PoolInstance, error) {
	var result []PoolInstance
	cursor := ""
	for page := 0; page < 20; page++ {
		query := url.Values{"tag": {poolTag}, "per_page": {"100"}}
		if cursor != "" {
			query.Set("cursor", cursor)
		}
		var reply struct {
			Instances []vultrInstance `json:"instances"`
			Meta      struct {
				Links struct {
					Next string `json:"next"`
				} `json:"links"`
			} `json:"meta"`
		}
		if err := v.do(ctx, http.MethodGet, "/instances?"+query.Encode(), nil, &reply); err != nil {
			return nil, err
		}
		for _, raw := range reply.Instances {
			if instance := v.instance(raw); managedPoolInstance(instance) {
				result = append(result, instance)
			}
		}
		if cursor = reply.Meta.Links.Next; cursor == "" {
			return result, nil
		}
	}
	return result, nil
}

func (v *VultrPool) Create(ctx context.Context, label string) (PoolInstance, error) {
	candidate := PoolInstance{Label: label, Tags: []string{poolTag}}
	if !managedPoolInstance(candidate) {
		return PoolInstance{}, errors.New("refusing to create an unmanaged instance")
	}
	body := map[string]any{"region": v.Region, "plan": v.Plan, "snapshot_id": v.SnapshotID,
		"label": label, "hostname": label, "tags": []string{poolTag}, "backups": "disabled"}
	if v.UserData != "" {
		body["user_data"] = v.UserData
	}
	var reply struct {
		Instance vultrInstance `json:"instance"`
	}
	if err := v.do(ctx, http.MethodPost, "/instances", body, &reply); err != nil {
		return PoolInstance{}, err
	}
	return v.instance(reply.Instance), nil
}

func (v *VultrPool) Destroy(ctx context.Context, instance PoolInstance) error {
	if !managedPoolInstance(instance) || !vultrIDPattern.MatchString(instance.ID) {
		return errors.New("refusing to destroy an unmanaged instance")
	}
	var current struct {
		Instance vultrInstance `json:"instance"`
	}
	if err := v.do(ctx, http.MethodGet, "/instances/"+instance.ID, nil, &current); err != nil {
		return err
	}
	live := v.instance(current.Instance)
	if live.ID != instance.ID || !managedPoolInstance(live) || live.Label != instance.Label {
		return errors.New("refusing to destroy: the instance is not a managed burst runner")
	}
	return v.do(ctx, http.MethodDelete, "/instances/"+instance.ID, nil, nil)
}
