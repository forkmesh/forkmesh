package control

const RelaySinkKey = "control.relaySink"

func (m *Module) SetRelaySender(send func(frame map[string]any) bool) {
	if send == nil {
		m.relaySender.Store(nil)
		return
	}
	m.relaySender.Store(&send)
}

func (m *Module) RelayOwned() bool { return m.relaySender.Load() != nil }

func (m *Module) RelayIn(frame map[string]any) {
	if !m.running.Load() || frame == nil {
		return
	}
	m.post(func() { m.relayFrame(frame) })
}

func (m *Module) RelayRestored() {
	if !m.running.Load() {
		return
	}
	m.post(func() {
		m.relay.restored(nil)
		m.relayRestored()
	})
}
