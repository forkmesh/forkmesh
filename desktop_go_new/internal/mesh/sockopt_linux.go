package mesh

import (
	"net"
	"syscall"
)

const tcpUserTimeout = 0x12

func reusePort(fd int) {}

func multicastTTLAndLoop(fd int) {
	_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_IP, syscall.IP_MULTICAST_TTL, 1)
	_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_IP, syscall.IP_MULTICAST_LOOP, 1)
}

func setUnackedTimeout(conn *net.TCPConn, ms int) {
	raw, err := conn.SyscallConn()
	if err != nil {
		return
	}
	_ = raw.Control(func(fd uintptr) {
		_ = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_TCP, tcpUserTimeout, ms)
	})
}
