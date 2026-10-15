package control

import "strings"

const savedInfoFile = "control-info.json"

type savedInfo struct {
	Name string `json:"name"`
	User string `json:"user"`
	Node string `json:"node"`
}

func (m *Module) applyInfo(info savedInfo) {
	m.name = leftUTF16(info.Name, maxNameChars)
	m.user = leftUTF16(info.User, maxNameChars)
	m.node = strings.ToLower(strings.TrimSpace(info.Node))
	m.core.Set("control.info", map[string]any{"name": m.name, "user": m.user, "node": m.node})
}

func (m *Module) rememberInfo() {
	info, ok := m.call(func() any { return savedInfo{Name: m.name, User: m.user, Node: m.node} }).(savedInfo)
	if !ok {
		return
	}
	if info.Node == "" && info.User == "" && info.Name == "" {
		if err := m.core.Forget(savedInfoFile); err != nil {
			m.core.Logger.Printf("control: could not remove the saved node name: %v", err)
		}
		return
	}
	if err := m.core.Remember(savedInfoFile, info); err != nil {
		m.core.Logger.Printf("control: could not save the node name: %v", err)
	}
}

func (m *Module) recallInfo() {
	var info savedInfo
	found, err := m.core.Recall(savedInfoFile, &info)
	if err != nil {
		m.core.Logger.Printf("control: ignoring the unreadable saved node name: %v", err)
		return
	}
	if found {
		m.applyInfo(info)
	}
}
