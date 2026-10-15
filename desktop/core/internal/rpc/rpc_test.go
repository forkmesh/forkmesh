package rpc

import (
	"context"
	"testing"
)

func TestRoutingAndDelegation(t *testing.T) {
	r := NewRegistry()
	r.HandleFamily("files.", "a", func(ctx context.Context, req *Request) (any, error) { return "family", nil })
	r.HandleFamily("files.browse.", "b", func(ctx context.Context, req *Request) (any, error) { return "browse", nil })
	r.Handle("files.browse.read", "c", func(ctx context.Context, req *Request) (any, error) { return nil, ErrDelegate })
	r.SetFallback(func(ctx context.Context, req *Request) (any, error) { return "proxy", nil })
	cases := map[string]string{
		"files.begin":       "family",
		"files.browse.list": "browse",
		"files.browse.read": "proxy",
		"agents.list":       "proxy",
	}
	for method, want := range cases {
		got, err := r.Dispatch(context.Background(), &Request{Method: method})
		if err != nil || got != want {
			t.Errorf("%s: got %v %v, want %s", method, got, err, want)
		}
	}
	if r.FamilyFor("files.browse.x") != "files.browse." {
		t.Fatal("longest prefix")
	}
	if r.HandleFamily("nodot", "x", func(ctx context.Context, req *Request) (any, error) { return nil, nil }) {
		t.Fatal("prefix without a dot accepted")
	}
	bare := NewRegistry()
	if _, err := bare.Dispatch(context.Background(), &Request{Method: "x"}); Code(err) != "unsupported" {
		t.Fatalf("no fallback: %v", err)
	}
}

func TestDelegateWithReplacesParams(t *testing.T) {
	r := NewRegistry()
	r.Handle("agentops.merge", "m", func(ctx context.Context, req *Request) (any, error) {
		return nil, DelegateWith([]byte(`{"id":3,"_coreConfirmed":"x"}`))
	})
	r.SetFallback(func(ctx context.Context, req *Request) (any, error) { return string(req.Params), nil })
	got, err := r.Dispatch(context.Background(), &Request{Method: "agentops.merge", Params: []byte(`{"id":3}`)})
	if err != nil || got != `{"id":3,"_coreConfirmed":"x"}` {
		t.Fatalf("got %v %v", got, err)
	}
	err = DelegateWith([]byte(`{}`))
	if !Delegated(err) || Code(err) != "delegate" {
		t.Fatalf("a delegation with params is a delegation: %v", err)
	}
	if params, ok := DelegatedParams(ErrDelegate); ok || params != nil {
		t.Fatal("the plain delegation carries no params")
	}
}
