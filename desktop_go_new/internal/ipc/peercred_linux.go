//go:build linux

package ipc

import (
	"net"
	"os"
	"syscall"
)

func peerCred(raw net.Conn) (*syscall.Ucred, bool) {
	unix, ok := raw.(*net.UnixConn)
	if !ok {
		return nil, false
	}
	control, err := unix.SyscallConn()
	if err != nil {
		return nil, false
	}
	var cred *syscall.Ucred
	var credErr error
	if err := control.Control(func(fd uintptr) {
		cred, credErr = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	}); err != nil || credErr != nil || cred == nil {
		return nil, false
	}
	return cred, true
}

func peerPID(raw net.Conn) (int, bool) {
	cred, ok := peerCred(raw)
	if !ok {
		return 0, false
	}
	return int(cred.Pid), true
}

func sameUserPeer(raw net.Conn) bool {
	if _, isUnix := raw.(*net.UnixConn); !isUnix {
		return true
	}
	cred, ok := peerCred(raw)
	return ok && int(cred.Uid) == os.Getuid()
}
