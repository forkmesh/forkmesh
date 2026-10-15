//go:build linux

package ipc

import (
	"net"
	"syscall"
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
	var cred *syscall.Ucred
	var credErr error
	if err := control.Control(func(fd uintptr) {
		cred, credErr = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	}); err != nil || credErr != nil || cred == nil {
		return 0, false
	}
	return int(cred.Pid), true
}
