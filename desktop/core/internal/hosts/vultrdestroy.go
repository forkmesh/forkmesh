package hosts

import (
	"context"
	"fmt"
	"net/http"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const unwireTimeout = 10 * time.Second

func (m *Module) destroyVultrHost(ctx context.Context, name, formKey, origin string, done func(ok bool, message string)) *Job {
	report := func(job *Job, text string, finished, ok bool) {
		m.setStatus("install", text)
		if !finished {
			return
		}
		if job != nil {
			m.finishJob(job, ok, text, nil)
		}
		if done != nil {
			done(ok, text)
		}
	}
	host, index := m.savedHostExact(name)
	if index < 0 {
		report(nil, fmt.Sprintf("No saved host is named \"%s\".", name), true, false)
		return nil
	}
	ip := jsonString(host["ip"])
	apiKey, _ := m.vultrAPIKey(formKey)
	if apiKey == "" {
		report(nil, "Enter your Vultr API key above (or store it as a VULTR_API_KEY variable) to destroy a server.", true, false)
		return nil
	}
	recorded := savedHostVultrInstanceID(host)
	job := m.startJob("destroy", name, "", origin, installChannel)
	go func() {
		runCtx := job.Context()
		instanceID := recorded
		if instanceID == "" {
			report(job, fmt.Sprintf("Looking up \"%s\" on Vultr…", name), false, false)
			result, failure := m.vultrCall(runCtx, apiKey, http.MethodGet, "/v2/instances?per_page=500", nil)
			if failure != "" {
				report(job, fmt.Sprintf("Could not destroy \"%s\": %s", name, failure), true, false)
				return
			}
			instanceID = vultrInstanceIDForAddress(jsonObjects(result["instances"]), ip)
			if instanceID == "" {
				report(job, fmt.Sprintf("No single Vultr instance matches \"%s\" (%s), so nothing was destroyed. Delete it from the Vultr panel instead.", name, ip), true, false)
				return
			}
		}
		if invalid := validateVultrDestroyRequest(apiKey, instanceID); invalid != "" {
			report(job, invalid, true, false)
			return
		}
		report(job, fmt.Sprintf("Destroying \"%s\" on Vultr…", name), false, false)
		m.appendOutput(installChannel, fmt.Sprintf("Destroying Vultr instance %s (\"%s\")…\n", instanceID, name))
		if _, failure := m.vultrCall(runCtx, apiKey, http.MethodDelete, "/v2/instances/"+instanceID, nil); failure != "" {
			m.appendOutput(installChannel, fmt.Sprintf("Vultr destroy failed: %s\n", failure))
			report(job, fmt.Sprintf("Could not destroy \"%s\": %s", name, failure), true, false)
			return
		}
		m.appendOutput(installChannel, fmt.Sprintf("Vultr instance %s destroyed.\n", instanceID))
		if unwired := m.unwireNode(runCtx, name); unwired > 0 {
			plural := "s"
			if unwired == 1 {
				plural = ""
			}
			m.appendOutput(installChannel, fmt.Sprintf("Removed %d git remote%s that pointed at \"%s\".\n", unwired, plural, name))
		}
		if _, at := m.savedHostExact(name); at >= 0 {
			m.forgetHostAt(at)
		}
		report(job, fmt.Sprintf("Destroyed \"%s\" on Vultr and removed it from this list.", name), true, true)
	}()
	return job
}

func (m *Module) handleDestroy(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Name   string `json:"name"`
		APIKey string `json:"apiKey"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	message := ""
	job := m.destroyVultrHost(m.ctx, params.Name, params.APIKey, "desktop", func(ok bool, text string) {
		if !ok {
			message = text
		}
	})
	if job == nil {
		return map[string]any{"started": false, "job": "", "message": message}, nil
	}
	return map[string]any{"started": true, "job": job.ID, "message": ""}, nil
}
