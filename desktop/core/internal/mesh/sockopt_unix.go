//go:build linux || darwin || freebsd || netbsd || openbsd || dragonfly

package mesh

import (
	"net"
	"syscall"
)

func controlReuse(network, address string, raw syscall.RawConn) error {
	var failure error
	err := raw.Control(func(fd uintptr) {
		failure = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		if failure == nil {
			reusePort(int(fd))
		}
	})
	if err != nil {
		return err
	}
	return failure
}

func setupDiscovery(conn *net.UDPConn) {
	raw, err := conn.SyscallConn()
	if err != nil {
		return
	}
	_ = raw.Control(func(fd uintptr) {
		_ = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_BROADCAST, 1)
		multicastTTLAndLoop(int(fd))
	})
}

func joinGroup(conn *net.UDPConn, group, local net.IP) error {
	raw, err := conn.SyscallConn()
	if err != nil {
		return err
	}
	var failure error
	err = raw.Control(func(fd uintptr) {
		request := &syscall.IPMreq{}
		copy(request.Multiaddr[:], group.To4())
		if local != nil {
			copy(request.Interface[:], local.To4())
		}
		failure = syscall.SetsockoptIPMreq(int(fd), syscall.IPPROTO_IP, syscall.IP_ADD_MEMBERSHIP, request)
	})
	if err != nil {
		return err
	}
	return failure
}

func setMulticastInterface(conn *net.UDPConn, local net.IP) error {
	raw, err := conn.SyscallConn()
	if err != nil {
		return err
	}
	var failure error
	err = raw.Control(func(fd uintptr) {
		var address [4]byte
		copy(address[:], local.To4())
		failure = syscall.SetsockoptInet4Addr(int(fd), syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF, address)
	})
	if err != nil {
		return err
	}
	return failure
}
