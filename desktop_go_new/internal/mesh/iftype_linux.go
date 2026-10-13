package mesh

import (
	"net"
	"os"
	"strings"
)

func ethernetLike(iface net.Interface) bool {
	raw, err := os.ReadFile("/sys/class/net/" + iface.Name + "/type")
	if err != nil {
		return false
	}
	switch strings.TrimSpace(string(raw)) {
	case "1", "801", "802", "803":
		return true
	}
	return false
}
