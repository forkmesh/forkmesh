//go:build !darwin

package fmmmobile

import "net"

func listenOn(family string, index int) (net.PacketConn, error) {
	return nil, ErrPlatform
}
