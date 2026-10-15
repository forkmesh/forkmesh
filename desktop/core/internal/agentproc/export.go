package agentproc

import "sort"

type Process struct {
	Key        string
	Pid        int
	ShimPid    int
	Running    bool
	Detachable bool
	Label      string
	Cwd        string
	Meta       map[string]any
}

func (m *Module) Processes() []Process {
	m.mu.Lock()
	out := make([]Process, 0, len(m.procs))
	for _, p := range m.procs {
		meta := make(map[string]any, len(p.meta))
		for key, value := range p.meta {
			meta[key] = value
		}
		out = append(out, Process{
			Key:        p.key,
			Pid:        p.run.Pid,
			ShimPid:    p.run.ShimPid,
			Running:    p.running(),
			Detachable: p.spec.Detachable,
			Label:      p.spec.Label,
			Cwd:        p.spec.Cwd,
			Meta:       meta,
		})
	}
	m.mu.Unlock()
	sort.Slice(out, func(i, j int) bool { return out[i].Key < out[j].Key })
	return out
}
