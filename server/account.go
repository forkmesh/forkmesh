package mirrornode

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// RegisterNodeAccount reserves a key-bound account for a provisioning desktop.
func RegisterNodeAccount(ctx context.Context, accountsURL, node, linkCode string, identity *Identity) error {
	base, err := url.Parse(strings.TrimRight(accountsURL, "/"))
	localHTTP := base.Scheme == "http" &&
		(base.Hostname() == "127.0.0.1" || base.Hostname() == "localhost" || base.Hostname() == "::1")
	if err != nil || (base.Scheme != "https" && !localHTTP) || base.Host == "" || !nodePattern.MatchString(node) {
		return errors.New("accounts URL or node name is invalid")
	}
	if len(linkCode) != 6 {
		return errors.New("link code must contain six digits")
	}
	for _, character := range linkCode {
		if character < '0' || character > '9' {
			return errors.New("link code must contain six digits")
		}
	}
	timestamp := strconv.FormatInt(time.Now().UnixMilli(), 10)
	reserveCanonical := []byte("forkmesh-reserve-v1\n" + node + "\n" + timestamp)
	reserve := map[string]any{"nodeName": node, "pubkey": identity.PublicKey(), "ts": timestamp, "sig": identity.Sign(reserveCanonical)}
	status, _, err := postAccountJSON(ctx, base, "reserve", reserve)
	if err != nil {
		return err
	}
	if status != http.StatusCreated && status != http.StatusConflict {
		return fmt.Errorf("account reserve returned HTTP %d", status)
	}
	finalTimestamp := strconv.FormatInt(time.Now().UnixMilli(), 10)
	finalCanonical := []byte("forkmesh-finalize-v1\n" + node + "\n\n" + finalTimestamp)
	finalize := map[string]any{
		"nodeName": node, "email": "", "password": "", "pubkey": identity.PublicKey(),
		"ts": finalTimestamp, "sig": identity.Sign(finalCanonical), "linkCode": linkCode,
	}
	status, body, err := postAccountJSON(ctx, base, "finalize", finalize)
	if err != nil {
		return err
	}
	if status == http.StatusCreated {
		return nil
	}
	// A reused node name (a host rebuilt under the same hostname) leaves the
	// account active and bound to the previous machine's key, so reserve is a
	// no-op conflict and finalize cannot verify this fresh key — HTTP 401
	// bad_signature. The installer link code is what ties this new key to the
	// user driving the install, so hand it to reclaim-node, which rotates the
	// node account's hosting key once that user's signed offer for the same
	// code is present (and parks this half until it is).
	if reclaimErr := reclaimNodeAccount(ctx, base, node, linkCode, identity); reclaimErr != nil {
		return fmt.Errorf("account finalize returned HTTP %d: %s; reclaiming the "+
			"existing node name failed: %v", status, boundedText(body, 200), reclaimErr)
	}
	return nil
}

// reclaimNodeAccount re-homes an already-active node account onto this node's
// key, proving the request with the installer link code plus a signature from
// the new key. Success means the link completed now, or that the node's half is
// parked for the installing desktop's offer.
func reclaimNodeAccount(ctx context.Context, base *url.URL, node, linkCode string, identity *Identity) error {
	timestamp := strconv.FormatInt(time.Now().UnixMilli(), 10)
	pubkey := identity.PublicKey()
	canonical := []byte("forkmesh-reclaim-node-v1\n" + node + "\n" + pubkey + "\n" +
		linkCode + "\n" + timestamp)
	reclaim := map[string]any{
		"nodeName": node, "pubkey": pubkey, "linkCode": linkCode,
		"ts": timestamp, "sig": identity.Sign(canonical),
	}
	status, body, err := postAccountJSON(ctx, base, "reclaim-node", reclaim)
	if err != nil {
		return err
	}
	var parsed struct {
		OK      bool `json:"ok"`
		Linked  bool `json:"linked"`
		Pending bool `json:"pending"`
	}
	_ = json.Unmarshal(body, &parsed)
	if status == http.StatusOK && parsed.OK && parsed.Linked {
		return nil
	}
	if status == http.StatusAccepted && parsed.Pending {
		return nil
	}
	return fmt.Errorf("account reclaim-node returned HTTP %d: %s", status, boundedText(body, 200))
}

func postAccountJSON(ctx context.Context, base *url.URL, leaf string, value any) (int, []byte, error) {
	body, err := json.Marshal(value)
	if err != nil {
		return 0, nil, err
	}
	target := *base
	target.Path = strings.TrimRight(base.Path, "/") + "/" + leaf
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, target.String(), bytes.NewReader(body))
	if err != nil {
		return 0, nil, err
	}
	request.Header.Set("Content-Type", "application/json")
	client := &http.Client{Timeout: 20 * time.Second}
	response, err := client.Do(request)
	if err != nil {
		return 0, nil, err
	}
	defer response.Body.Close()
	responseBody, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
	return response.StatusCode, responseBody, nil
}
