package agentmodel

import (
	"context"
	"encoding/json"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

func (m *Module) wrapTranscript(core *daemon.Core) {
	original, ok := core.Registry.Resolve("agents.transcript")
	if !ok {
		original = func(ctx context.Context, req *rpc.Request) (any, error) { return nil, rpc.ErrDelegate }
	}
	core.Registry.Handle("agents.transcript", "agentrun", func(ctx context.Context, req *rpc.Request) (any, error) {
		params := paramsObject(req)
		id := params.Get("id").Int()
		_, pushed := m.snapshot()
		_, credentialed := m.credentials()
		if id < 0 && pushed && credentialed {
			since := int64(-1)
			if params.Has("since") {
				since = int64(params.Get("since").Float())
			}
			result := m.readOutside(id, since)
			if result.err != "" {
				return nil, rpc.Fail(result.err)
			}
			return json.RawMessage(result.payload.Compact()), nil
		}
		result, err := original(ctx, req)
		if err != nil || id <= 0 || !pushed {
			return result, err
		}
		reply, isMap := result.(map[string]any)
		known := m.knownImages(id)
		if !isMap || len(known) == 0 {
			return result, err
		}
		session, store, found := m.ownSession(id)
		if !found {
			return result, err
		}
		reply["attachments"] = json.RawMessage(attachmentsJSON(m.attachmentEntries(
			m.sessionAttachmentPaths(store, session, known), id, maxAttachmentEntries)).Compact())
		return reply, nil
	})
}
