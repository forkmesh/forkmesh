package phonefamily

import (
	"context"
	"strings"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

type Gate struct {
	Core   *daemon.Core
	Owners func() []string
}

func (g *Gate) settings() *qsettings.Settings {
	if g == nil || g.Core == nil || g.Core.Settings == nil {
		return qsettings.Parse(nil)
	}
	return g.Core.Settings.Get()
}

func (g *Gate) Names() []string {
	if g.Owners != nil {
		return g.Owners()
	}
	var names []string
	if g.Core != nil {
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
	}
	return append(names, AccountOwner(g.settings()))
}

func OwnerMayManage(deviceUser, account string) bool {
	device := strings.TrimSpace(deviceUser)
	account = strings.TrimSpace(account)
	return device != "" && account != "" && strings.EqualFold(device, account)
}

func (g *Gate) IsOwner(device rpc.Device) bool {
	if device.Key == "" {
		return false
	}
	for _, name := range g.Names() {
		if OwnerMayManage(device.User, name) {
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

func DeviceOf(req *rpc.Request) rpc.Device {
	if req == nil || req.Peer == nil {
		return rpc.Device{}
	}
	return req.Peer.Device()
}

func Holder(req *rpc.Request) string {
	if req == nil || req.Peer == nil {
		return ""
	}
	return req.Peer.Device().Key + "#" + req.Peer.ID()
}

func DeviceLabel(req *rpc.Request) string {
	if req == nil || req.Peer == nil {
		return "a phone"
	}
	device := req.Peer.Device()
	label := device.Name
	if label == "" {
		label = "Phone"
	}
	if device.User != "" {
		label += " (" + device.User + ")"
	}
	if req.Peer.Transport() == rpc.Relay {
		label += ", through the relay"
	} else {
		label += ", on the local network"
	}
	return label
}

func BroadcastPhones(core *daemon.Core, event string, data any) {
	if core == nil || core.Hub == nil {
		return
	}
	if data == nil {
		data = map[string]any{}
	}
	core.Hub.Broadcast(event, data, rpc.Remote)
}

func DesktopUnavailable(err error) bool {
	switch rpc.Code(err) {
	case "ui_gone", "ui_timeout", "desktop_unavailable", context.Canceled.Error(), context.DeadlineExceeded.Error():
		return true
	}
	return false
}
