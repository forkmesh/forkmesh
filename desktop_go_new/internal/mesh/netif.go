package mesh

import (
	"net"
	"sort"
	"strconv"
	"strings"
)

type IfAddr struct {
	IP        net.IP
	Prefix    int
	Broadcast net.IP
}

type Interface struct {
	Name      string
	Index     int
	Up        bool
	Running   bool
	Multicast bool
	Loopback  bool
	Sweepable bool
	Addrs     []IfAddr
}

var bridgePrefixes = []string{"docker", "br-", "veth", "virbr", "vboxnet", "vmnet", "lxcbr", "lxdbr", "cni", "podman", "flannel"}

func SystemInterfaces() []Interface {
	all, err := net.Interfaces()
	if err != nil {
		return nil
	}
	out := make([]Interface, 0, len(all))
	for _, iface := range all {
		entry := Interface{Name: iface.Name, Index: iface.Index,
			Up:        iface.Flags&net.FlagUp != 0,
			Running:   iface.Flags&net.FlagRunning != 0,
			Multicast: iface.Flags&net.FlagMulticast != 0,
			Loopback:  iface.Flags&net.FlagLoopback != 0}
		entry.Sweepable = ethernetLike(iface) && !bridgeName(iface.Name)
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			network, ok := addr.(*net.IPNet)
			if !ok {
				continue
			}
			ones, bits := network.Mask.Size()
			if bits == 0 {
				ones = -1
			}
			item := IfAddr{IP: network.IP, Prefix: ones}
			if v4 := network.IP.To4(); v4 != nil {
				item.IP = v4
				if iface.Flags&net.FlagBroadcast != 0 && ones >= 0 && ones <= 30 {
					mask := net.CIDRMask(ones, 32)
					broadcast := make(net.IP, 4)
					for i := range broadcast {
						broadcast[i] = v4[i] | ^mask[i]
					}
					item.Broadcast = broadcast
				}
			}
			entry.Addrs = append(entry.Addrs, item)
		}
		out = append(out, entry)
	}
	return out
}

func bridgeName(name string) bool {
	for _, prefix := range bridgePrefixes {
		if strings.HasPrefix(name, prefix) {
			return true
		}
	}
	return false
}

func meshInterfaces(all []Interface) []Interface {
	usable := []Interface{}
	for _, iface := range all {
		if !iface.Up || !iface.Running || !iface.Multicast {
			continue
		}
		for _, addr := range iface.Addrs {
			if IsLocalNetworkAddress(addr.IP) {
				usable = append(usable, iface)
				break
			}
		}
	}
	return usable
}

func addressSignature(all []Interface) string {
	parts := []string{}
	for _, iface := range meshInterfaces(all) {
		for _, addr := range iface.Addrs {
			if IsLocalNetworkAddress(addr.IP) {
				parts = append(parts, iface.Name+" "+addr.IP.String())
			}
		}
	}
	sort.Strings(parts)
	return strings.Join(parts, ",")
}

func firstIPv4(iface Interface) net.IP {
	for _, addr := range iface.Addrs {
		if v4 := addr.IP.To4(); v4 != nil {
			return v4
		}
	}
	return nil
}

func interfaceText(all []Interface) string {
	parts := []string{}
	for _, iface := range meshInterfaces(all) {
		for _, addr := range iface.Addrs {
			v4 := addr.IP.To4()
			if v4 == nil || !IsLocalNetworkAddress(v4) || v4[0] == 127 {
				continue
			}
			mark := ""
			if iface.Sweepable {
				mark = "*"
			}
			parts = append(parts, iface.Name+" "+v4.String()+"/"+strconv.Itoa(addr.Prefix)+mark)
		}
	}
	if len(parts) == 0 {
		return "no LAN interface"
	}
	return strings.Join(parts, ", ")
}

func allAddresses(all []Interface) map[string]bool {
	out := map[string]bool{}
	for _, iface := range all {
		for _, addr := range iface.Addrs {
			out[normalIP(addr.IP).String()] = true
		}
	}
	return out
}

func normalIP(ip net.IP) net.IP {
	if v4 := ip.To4(); v4 != nil {
		return v4
	}
	return ip
}
