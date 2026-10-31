//go:build darwin

package fmmmobile

import (
	"context"
	"net"
	"syscall"
)

func listenOn(family string, index int) (net.PacketConn, error) {
	if index <= 0 {
		return nil, ErrPlatform
	}
	lc := net.ListenConfig{Control: func(network, address string, rc syscall.RawConn) error {
		var serr error
		err := rc.Control(func(fd uintptr) {
			if family == "udp6" {
				serr = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IPV6, syscall.IPV6_BOUND_IF, index)
			} else {
				serr = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_BOUND_IF, index)
			}
		})
		if err != nil {
			return err
		}
		return serr
	}}
	addr := "0.0.0.0:0"
	if family == "udp6" {
		addr = "[::]:0"
	}
	return lc.ListenPacket(context.Background(), family, addr)
}
