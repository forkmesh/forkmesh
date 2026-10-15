package mesh

const savedStartFile = "mesh.json"

func (m *Module) remember(params startParams) {
	params.SealKey = ""
	if params == m.saved {
		return
	}
	m.saved = params
	if err := m.core.Remember(savedStartFile, params); err != nil {
		m.core.Logger.Printf("Local mesh: could not save the start settings: %v", err)
	}
}

func (m *Module) forget() {
	if err := m.core.Forget(savedStartFile); err != nil {
		m.core.Logger.Printf("Local mesh: could not remove the saved start settings: %v", err)
	}
}

func (m *Module) resume() {
	var params startParams
	found, err := m.core.Recall(savedStartFile, &params)
	if err != nil {
		m.core.Logger.Printf("Local mesh: ignoring unreadable saved start settings: %v", err)
		return
	}
	if !found || trimmed(params.NodeID) == "" {
		return
	}
	params.SealKey = ""
	if _, err := m.startMesh(params); err != nil {
		m.core.Logger.Printf("Local mesh: not starting without the desktop: %v", err)
		return
	}
	m.core.Logger.Printf("Local mesh: started with the settings the desktop last gave, without waiting for it")
}
