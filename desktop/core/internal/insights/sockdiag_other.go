//go:build !linux

package insights

func tcpSockets() []NetSocket {
	return []NetSocket{}
}
