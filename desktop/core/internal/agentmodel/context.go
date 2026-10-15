package agentmodel

import (
	"strconv"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const externalIDBase = -1000

type sessionLive struct {
	Unread    bool
	Terminal  string
	DiffStats qtjson.Value
	Worktree  bool
	Images    []string
}

type outsideSource struct {
	Label      string
	Cwd        string
	OtherUser  bool
	Terminal   bool
	Transcript bool
	Path       string
	Kind       string
	SourceCwd  string
}

type repository struct {
	Owner       string
	Name        string
	LocalPath   string
	MirrorPath  string
	PreviewOnly bool
	Active      bool
}

type desktopContext struct {
	Seq             int64
	Live            map[int]sessionLive
	Outside         qtjson.Value
	OutsideSources  map[int]outsideSource
	Repositories    []repository
	Folders         qtjson.Value
	Providers       qtjson.Value
	DefaultProvider string
	DefaultMode     string
	Running         int
	RunningIDs      []int
	Queue           []int
	Limit           int
	Approvals       qtjson.Value
	ApprovalIndex   map[string]approvalRef
	Grants          qtjson.Value
	ExternalIDBase  int
	DetailRepo      int
	InFlight        map[int]bool
	Extra           qtjson.Value
}

type approvalRef struct {
	Session int
	Request string
}

func intList(value qtjson.Value) []int {
	out := []int{}
	for _, item := range value.Items() {
		out = append(out, item.Int())
	}
	return out
}

func stringList(value qtjson.Value) []string {
	out := []string{}
	for _, item := range value.Items() {
		if item.IsString() {
			out = append(out, item.Str())
		}
	}
	return out
}

func parseContext(params qtjson.Value) desktopContext {
	ctx := desktopContext{
		Seq:             params.Get("seq").Int64(),
		Live:            map[int]sessionLive{},
		Outside:         params.Get("outside"),
		Folders:         params.Get("folders"),
		OutsideSources:  map[int]outsideSource{},
		Providers:       params.Get("providers"),
		DefaultProvider: params.Get("defaultProvider").Str(),
		DefaultMode:     params.Get("defaultMode").Str(),
		Running:         params.Get("running").Int(),
		RunningIDs:      intList(params.Get("runningIds")),
		Queue:           intList(params.Get("queue")),
		Limit:           params.Get("limit").IntOr(0),
		Approvals:       params.Get("approvals"),
		ApprovalIndex:   map[string]approvalRef{},
		Grants:          params.Get("grants"),
		ExternalIDBase:  params.Get("externalIdBase").IntOr(externalIDBase),
		DetailRepo:      params.Get("detailRepo").IntOr(-1),
		InFlight:        map[int]bool{},
		Extra:           params.Get("extra"),
	}
	if !ctx.Outside.IsArray() {
		ctx.Outside = qtjson.NewArray()
	}
	if !ctx.Providers.IsArray() {
		ctx.Providers = qtjson.NewArray()
	}
	if !ctx.Folders.IsArray() {
		ctx.Folders = qtjson.NewArray()
	}
	if !ctx.Approvals.IsArray() {
		ctx.Approvals = qtjson.NewArray()
	}
	for _, row := range params.Get("sessions").Items() {
		id := row.Get("id").Int()
		if id == 0 {
			continue
		}
		live := sessionLive{Unread: row.Get("unread").BoolValue(), Terminal: row.Get("terminal").Str(),
			Images: stringList(row.Get("images"))}
		if stats := row.Get("diffStats"); stats.IsObject() {
			live.DiffStats = stats
			live.Worktree = row.Get("worktree").BoolValue()
		}
		ctx.Live[id] = live
	}
	for _, row := range params.Get("outsideSources").Items() {
		source := outsideSource{Label: row.Get("label").Str(), Cwd: row.Get("cwd").Str(),
			OtherUser: row.Get("otherUser").BoolValue(), Terminal: row.Get("terminal").BoolValue()}
		if transcript := row.Get("transcript"); transcript.IsObject() {
			source.Transcript = true
			source.Path = transcript.Get("path").Str()
			source.Kind = transcript.Get("kind").Str()
			source.SourceCwd = transcript.Get("cwd").Str()
		}
		ctx.OutsideSources[row.Get("id").Int()] = source
	}
	for _, row := range params.Get("repositories").Items() {
		ctx.Repositories = append(ctx.Repositories, repository{Owner: row.Get("owner").Str(), Name: row.Get("name").Str(),
			LocalPath: row.Get("localPath").Str(), MirrorPath: row.Get("mirrorPath").Str(),
			PreviewOnly: row.Get("previewOnly").BoolValue(), Active: row.Get("active").BoolValue()})
	}
	for _, id := range intList(params.Get("inFlight")) {
		ctx.InFlight[id] = true
	}
	for _, row := range params.Get("approvalIndex").Items() {
		ctx.ApprovalIndex[row.Get("id").Str()] = approvalRef{Session: row.Get("session").Int(), Request: row.Get("request").Str()}
	}
	return ctx
}

func (c *desktopContext) external(id int) bool {
	return id <= c.ExternalIDBase
}

func (c *desktopContext) repoIndexFor(owner, name string) int {
	for i, repo := range c.Repositories {
		if !repo.PreviewOnly && repo.Owner == owner && repo.Name == name && repo.Active {
			return i
		}
	}
	return -1
}

func (c *desktopContext) checkoutIndexFold(owner, name string) int {
	for i, repo := range c.Repositories {
		if equalFoldQt(repo.Owner, owner) && equalFoldQt(repo.Name, name) && repo.LocalPath != "" {
			return i
		}
	}
	return -1
}

func idKey(id int) string { return strconv.Itoa(id) }
