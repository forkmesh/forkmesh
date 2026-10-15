//go:build darwin || freebsd || netbsd || openbsd || dragonfly

package mesh

import (
	"net"
	"syscall"
)

func reusePort(fd int) {
	_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEPORT, 1)
}

func multicastTTLAndLoop(fd int) {
	_ = syscall.SetsockoptByte(fd, syscall.IPPROTO_IP, syscall.IP_MULTICAST_TTL, 1)
	_ = syscall.SetsockoptByte(fd, syscall.IPPROTO_IP, syscall.IP_MULTICAST_LOOP, 1)
}

func setUnackedTimeout(conn *net.TCPConn, ms int) {}
