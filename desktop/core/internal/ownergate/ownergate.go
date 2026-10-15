package ownergate

import (
	"context"
	"strings"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type Gate struct {
	Core   *daemon.Core
	Owners func() []string
}

func (g *Gate) Names() []string {
	if g.Owners != nil {
		return g.Owners()
	}
	if g.Core == nil {
		return nil
	}
	var names []string
	switch value := g.Core.Get("control.owners").(type) {
	case []string:
		names = append(names, value...)
	case []any:
		for _, item := range value {
			if text, ok := item.(string); ok {
				names = append(names, text)
			}
		}
	case string:
		names = append(names, value)
	}
	switch info := g.Core.Get("control.info").(type) {
	case map[string]any:
		if user, ok := info["user"].(string); ok {
			names = append(names, user)
		}
	case map[string]string:
		names = append(names, info["user"])
	}
	if g.Core.Settings != nil {
		names = append(names, g.Core.Settings.Get().String("account/nodeName", ""))
	}
	return names
}

func MayManage(deviceUser, account string) bool {
	device := strings.TrimSpace(deviceUser)
	account = strings.TrimSpace(account)
	return device != "" && account != "" && strings.EqualFold(device, account)
}

func (g *Gate) IsOwner(device rpc.Device) bool {
	if device.Key == "" {
		return false
	}
	for _, name := range g.Names() {
		if MayManage(device.User, name) {
			return true
		}
	}
	return false
}

func (g *Gate) Wrap(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer != nil && rpc.Remote(req.Peer) && !g.IsOwner(req.Peer.Device()) {
			return nil, rpc.Fail("not_owner")
		}
		return handler(ctx, req)
	}
}

func DesktopOnly(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return handler(ctx, req)
	}
}

func DeviceLabel(peer rpc.Peer) string {
	if peer == nil {
		return "a phone"
	}
	device := peer.Device()
	label := device.Name
	if label == "" {
		label = "Phone"
	}
	if device.User != "" {
		label += " (" + device.User + ")"
	}
	if peer.Transport() == rpc.Relay {
		label += ", through the relay"
	} else {
		label += ", on the local network"
	}
	return label
}

func DeviceKey(peer rpc.Peer) string {
	if peer == nil {
		return ""
	}
	return peer.Device().Key
}
