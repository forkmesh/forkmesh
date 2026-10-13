//go:build !linux

package mesh

import (
	"net"
	"strings"
)

var virtualPrefixes = []string{"bridge", "utun", "ipsec", "awdl", "llw", "anpi", "gif", "stf", "p2p", "ap", "lo", "pdp_ip", "vEthernet", "VirtualBox", "VMware"}

func ethernetLike(iface net.Interface) bool {
	if len(iface.HardwareAddr) != 6 || iface.Flags&(net.FlagLoopback|net.FlagPointToPoint) != 0 {
		return false
	}
	for _, prefix := range virtualPrefixes {
		if strings.HasPrefix(iface.Name, prefix) {
			return false
		}
	}
	return true
}
