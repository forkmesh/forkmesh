package agentrun

func (m *Module) Salt() []byte {
	if m == nil {
		return nil
	}
	return m.salt
}

func (m *Module) CredentialValues() []string {
	if m == nil {
		return nil
	}
	return m.credentialValues()
}

func (m *Module) HaveCredentials() bool {
	return m != nil && m.haveCredentials()
}
