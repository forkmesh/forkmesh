package hosts

import (
	"context"
	"encoding/json"
	"io"
	"math"
	"net/http"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

const vultrAPIBase = "https://api.vultr.com"

var preferredUSRegions = []string{"ewr", "atl", "ord", "dfw", "mia", "lax", "sea", "sjc", "hon"}

func planLocations(plan map[string]any) []string {
	raw, _ := plan["locations"].([]any)
	out := make([]string, 0, len(raw))
	for _, value := range raw {
		out = append(out, jsonString(value))
	}
	return out
}

func vultrPlanHasIPv4(plan map[string]any) bool {
	id := strings.ToLower(strings.TrimSpace(jsonString(plan["id"])))
	return id != "" && !strings.HasSuffix(id, "-v6") && !strings.Contains(id, "-v6-")
}

func planHasUSLocation(plan map[string]any) bool {
	for _, location := range planLocations(plan) {
		region := strings.ToLower(strings.TrimSpace(location))
		for _, us := range preferredUSRegions {
			if region == us {
				return true
			}
		}
	}
	return false
}

func cheapestVultrPlan(plans []map[string]any) map[string]any {
	const minimumRAM = 1024.0
	var best map[string]any
	for _, plan := range plans {
		cost := jsonDouble(plan["monthly_cost"])
		ram := jsonDouble(plan["ram"])
		id := jsonString(plan["id"])
		if id == "" || math.IsInf(cost, 0) || math.IsNaN(cost) || cost <= 0 || math.IsInf(ram, 0) || math.IsNaN(ram) ||
			ram < minimumRAM || !vultrPlanHasIPv4(plan) || !planHasUSLocation(plan) {
			continue
		}
		if best == nil {
			best = plan
			continue
		}
		bestCost := jsonDouble(best["monthly_cost"])
		if cost < bestCost {
			best = plan
			continue
		}
		if cost > bestCost {
			continue
		}
		bestRAM := jsonDouble(best["ram"])
		if ram > bestRAM || (ram == bestRAM && id < jsonString(best["id"])) {
			best = plan
		}
	}
	return best
}

func vultrPlanRegions(plan map[string]any) []string {
	present := map[string]bool{}
	for _, location := range planLocations(plan) {
		if region := strings.ToLower(strings.TrimSpace(location)); region != "" {
			present[region] = true
		}
	}
	out := []string{}
	for _, region := range preferredUSRegions {
		if present[region] {
			out = append(out, region)
		}
	}
	return out
}

func vultrPlanRegion(plan map[string]any) string {
	regions := vultrPlanRegions(plan)
	if len(regions) == 0 {
		return ""
	}
	return regions[0]
}

func vultrPlanChoices(plans []map[string]any) map[string]any {
	eligible := []map[string]any{}
	for _, plan := range plans {
		if cheapestVultrPlan([]map[string]any{plan}) != nil {
			eligible = append(eligible, plan)
		}
	}
	sort.SliceStable(eligible, func(i, j int) bool {
		ci, cj := jsonDouble(eligible[i]["monthly_cost"]), jsonDouble(eligible[j]["monthly_cost"])
		if ci != cj {
			return ci < cj
		}
		return jsonString(eligible[i]["id"]) < jsonString(eligible[j]["id"])
	})
	out := make([]map[string]any, 0, len(eligible))
	for _, plan := range eligible {
		out = append(out, map[string]any{
			"id":          jsonString(plan["id"]),
			"monthlyCost": jsonDouble(plan["monthly_cost"]),
			"ramMb":       jsonDouble(plan["ram"]),
			"diskGb":      jsonDouble(plan["disk"]),
			"vcpus":       jsonDouble(plan["vcpu_count"]),
			"regions":     vultrPlanRegions(plan),
		})
	}
	best := cheapestVultrPlan(plans)
	return map[string]any{
		"plans":         out,
		"defaultPlan":   jsonString(best["id"]),
		"defaultRegion": vultrPlanRegion(best),
	}
}

var mirrorNamePattern = regexp.MustCompile(`^mirror-?(\d{1,4})$`)

func nextMirrorNodeName(existing []string) string {
	used := map[string]bool{}
	highest := 0
	for _, name := range existing {
		normalized := strings.ToLower(strings.TrimSpace(name))
		if normalized == "" {
			continue
		}
		used[normalized] = true
		if match := mirrorNamePattern.FindStringSubmatch(normalized); match != nil {
			if n, err := strconv.Atoi(match[1]); err == nil && n > highest {
				highest = n
			}
		}
	}
	start := highest + 1
	if start < 1 {
		start = 1
	}
	for i := start; i <= 9999; i++ {
		candidate := "mirror" + strconv.Itoa(i)
		if !used[candidate] {
			return candidate
		}
	}
	return ""
}

func (m *Module) vultrPlans(ctx context.Context, apiKey string) ([]map[string]any, error) {
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, m.vultrBase()+"/v2/plans?per_page=500", nil)
	if err != nil {
		return nil, err
	}
	request.Header.Set("Authorization", "Bearer "+apiKey)
	request.Header.Set("Content-Type", "application/json")
	response, err := m.httpClient().Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return nil, &httpStatusError{status: response.StatusCode}
	}
	var decoded struct {
		Plans []map[string]any `json:"plans"`
	}
	_ = json.Unmarshal(body, &decoded)
	return decoded.Plans, nil
}

type httpStatusError struct{ status int }

func (e *httpStatusError) Error() string {
	return "Vultr API error (HTTP " + strconv.Itoa(e.status) + ")"
}
