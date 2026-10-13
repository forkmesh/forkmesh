//go:build darwin

package ipc

import (
	"net"
	"syscall"
)

const (
	solLocal     = 0
	localPeerPID = 0x002
)

func peerPID(raw net.Conn) (int, bool) {
	unix, ok := raw.(*net.UnixConn)
	if !ok {
		return 0, false
	}
	control, err := unix.SyscallConn()
	if err != nil {
		return 0, false
	}
	pid := 0
	var pidErr error
	if err := control.Control(func(fd uintptr) {
		pid, pidErr = syscall.GetsockoptInt(int(fd), solLocal, localPeerPID)
	}); err != nil || pidErr != nil || pid <= 0 {
		return 0, false
	}
	return pid, true
}
