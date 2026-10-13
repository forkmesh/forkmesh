//go:build !linux && !darwin

package ipc

import "net"

func peerPID(raw net.Conn) (int, bool) { return 0, false }
