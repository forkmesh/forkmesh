package mirrornode

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
)

func TestRegisterNodeAccountReservesAndFinalizesLinkCode(t *testing.T) {
	identity, err := GenerateIdentity(filepath.Join(t.TempDir(), "identity.pem"))
	if err != nil {
		t.Fatal(err)
	}
	var leaves []string
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		leaves = append(leaves, request.URL.Path)
		var body map[string]any
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Error(err)
		}
		if body["nodeName"] != "mirror17" || body["pubkey"] != identity.PublicKey() {
			t.Errorf("unexpected request: %+v", body)
		}
		if request.URL.Path == "/api/accounts/finalize" {
			if body["linkCode"] != "123456" {
				t.Errorf("missing link code: %+v", body)
			}
			writer.WriteHeader(http.StatusCreated)
			return
		}
		writer.WriteHeader(http.StatusCreated)
	}))
	defer server.Close()
	if err := RegisterNodeAccount(context.Background(), server.URL+"/api/accounts", "mirror17", "123456", identity); err != nil {
		t.Fatal(err)
	}
	if len(leaves) != 2 || leaves[0] != "/api/accounts/reserve" || leaves[1] != "/api/accounts/finalize" {
		t.Fatalf("unexpected calls: %+v", leaves)
	}
}

func TestRegisterNodeAccountReclaimsReusedNodeName(t *testing.T) {
	identity, err := GenerateIdentity(filepath.Join(t.TempDir(), "identity.pem"))
	if err != nil {
		t.Fatal(err)
	}
	for _, reclaimStatus := range []int{http.StatusOK, http.StatusAccepted} {
		var leaves []string
		server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
			leaves = append(leaves, request.URL.Path)
			var body map[string]any
			if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
				t.Error(err)
			}
			switch request.URL.Path {
			case "/api/accounts/reserve":
				// The name is already active under the previous host's key.
				writer.WriteHeader(http.StatusConflict)
			case "/api/accounts/finalize":
				writer.WriteHeader(http.StatusUnauthorized)
				writer.Write([]byte(`{"error":"bad_signature"}`))
			case "/api/accounts/reclaim-node":
				if body["linkCode"] != "123456" || body["pubkey"] != identity.PublicKey() {
					t.Errorf("unexpected reclaim request: %+v", body)
				}
				writer.WriteHeader(reclaimStatus)
				if reclaimStatus == http.StatusOK {
					writer.Write([]byte(`{"ok":true,"linked":true,"user":"jett"}`))
				} else {
					writer.Write([]byte(`{"ok":true,"linked":false,"pending":true}`))
				}
			default:
				t.Errorf("unexpected leaf: %s", request.URL.Path)
			}
		}))
		err := RegisterNodeAccount(context.Background(), server.URL+"/api/accounts", "mirror20", "123456", identity)
		server.Close()
		if err != nil {
			t.Fatalf("reclaim status %d: %v", reclaimStatus, err)
		}
		if len(leaves) != 3 || leaves[2] != "/api/accounts/reclaim-node" {
			t.Fatalf("unexpected calls: %+v", leaves)
		}
	}
}

func TestRegisterNodeAccountReportsFailedReclaim(t *testing.T) {
	identity, err := GenerateIdentity(filepath.Join(t.TempDir(), "identity.pem"))
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/api/accounts/reserve":
			writer.WriteHeader(http.StatusConflict)
		case "/api/accounts/reclaim-node":
			writer.WriteHeader(http.StatusForbidden)
			writer.Write([]byte(`{"error":"not_a_node"}`))
		default:
			writer.WriteHeader(http.StatusUnauthorized)
			writer.Write([]byte(`{"error":"bad_signature"}`))
		}
	}))
	defer server.Close()
	err = RegisterNodeAccount(context.Background(), server.URL+"/api/accounts", "mirror20", "123456", identity)
	if err == nil || !strings.Contains(err.Error(), "not_a_node") {
		t.Fatalf("expected the reclaim failure to surface, got %v", err)
	}
}

func TestRegisterNodeAccountRejectsInvalidLinkCode(t *testing.T) {
	identity, err := GenerateIdentity(filepath.Join(t.TempDir(), "identity.pem"))
	if err != nil {
		t.Fatal(err)
	}
	if err := RegisterNodeAccount(context.Background(), "https://example.test/api/accounts", "mirror17", "12x456", identity); err == nil {
		t.Fatal("invalid link code accepted")
	}
}
