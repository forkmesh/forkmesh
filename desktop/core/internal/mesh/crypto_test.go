package mesh

import (
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

const (
	vectorRoom      = "world-general"
	senderPrivate   = "UE5ZIyTdbSCtuDSqkI3g5TnXBMw1nx4Oz1BfmvE6CGc"
	receiverPrivate = "cJMcO0LKgE1B6DRDn8z2mCcgt-cKyuWEtafYL2hFjHk"
	senderKeyID     = "SQAm1SbhWWBxNiSt"
	receiverKeyID   = "PA22MdT_F-KVxa5L"
	senderPub       = "SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTw"
	meshRoomKeyHex  = "66e8edec89c7c0f05e5bb3d7f639cb5e070102e2b88d83c3831891edb3b655ac"
	beaconPlainText = `{"control":true,"id":"node-ada-1","name":"ada","nodeName":"workshop","platform":"linux","port":4242,"t":"forkmesh-lan-mesh","ts":1750000000000,"v":1}`
	beaconEnvelope  = `{"body":"N++hZhCRDTrfkFHsvun+KJ/hU6NjhNvrTmyksIqX1B8G6o6nzpygISGA9y9eS8Sfd8P2hDTQDey5I4dtfFhSf/Z4ZV1q7fwLdKbda024cVkCaPkosTeW3pbBprfPxJ6iFhAdVLjBlMfJcLkIaHowvfGi5wb0WnrXmisCWRzrhoUpmvnR69bNUOxPDudNFlFO/CP1XYc=","kind":"cipher","nonce":"AQIDBAUGBwgJCgsM","tag":"zBShZ9xsWuwspiynyNfqSQ==","v":1}`
)

func vectorPlain() Object {
	return Object{"type": "chat", "id": "seal-vector-2", "sender": "ada", "text": "automatic beyond the relay"}
}

func vectorFrame() Object {
	return Object{
		"type":  "sealed",
		"v":     float64(2),
		"kid":   senderKeyID,
		"pub":   senderPub,
		"nonce": "43+zI9tUg9t5SdVG",
		"tag":   "D9qgU0oVQpoOY05RaI3xDg==",
		"body":  "Jn79ws7IdrSi5kFmmLHAB7yUUuARxKFeLZKdzMQBoHbghdPyQgEq8OZanyL0hTh/TYbavVieT2DhVLLZq2F0kH1yeAP7JdwPDqPysTZCETMtZYDTKBLr",
		"keys": []any{
			Object{"kid": receiverKeyID, "nonce": "dIYd3AXET2IQhRjH", "tag": "wga2/Tmek1r/0u9oXq4ckw==", "key": "pNMivvZuaR6ZEu3V/GkZOVYDlSP4Lr4MtsG0mIwtOMs="},
			Object{"kid": senderKeyID, "nonce": "5vZE4C5wbgSbNENA", "tag": "eiRSoFEo1m5rBwQ0jSgiEw==", "key": "K7BObyf/R3vByKsOWMJQWiRDBb02WFDP3fy4cGaJ5O8="},
		},
	}
}

func urlKey(t *testing.T, text string) []byte {
	t.Helper()
	raw, err := base64.RawURLEncoding.DecodeString(text)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func mustSeal(t *testing.T, room string, private []byte) *RoomSeal {
	t.Helper()
	seal, err := NewRoomSeal(room, private)
	if err != nil {
		t.Fatal(err)
	}
	return seal
}

func roundTrip(t *testing.T, object Object) Object {
	t.Helper()
	parsed, ok := parseObject(marshal(object))
	if !ok {
		t.Fatalf("not an object: %s", marshal(object))
	}
	return parsed
}

func TestMeshRoomKeyMatchesPBKDF2Vector(t *testing.T) {
	key, err := RoomKey(RoomName, appRoomKey)
	if err != nil || hex.EncodeToString(key) != meshRoomKeyHex {
		t.Fatalf("room key %x %v", key, err)
	}
	if _, err := RoomKey("   ", appRoomKey); err == nil {
		t.Fatal("a blank room has no key")
	}
	padded, _ := RoomKey("  "+RoomName+"\n", appRoomKey)
	if hex.EncodeToString(padded) != meshRoomKeyHex {
		t.Fatal("the room name is trimmed, as saltForRoom sees it")
	}
}

func TestBeaconPlaintextIsQtCompactJSON(t *testing.T) {
	beacon := Beacon{NodeID: "node-ada-1", Name: "ada", NodeName: "workshop", Platform: "linux", Port: 4242,
		TimestampMs: 1750000000000, Control: true}
	if got := string(marshal(BeaconObject(beacon))); got != beaconPlainText {
		t.Fatalf("beacon plaintext\n got %s\nwant %s", got, beaconPlainText)
	}
}

func TestBeaconEnvelopeIsByteForByte(t *testing.T) {
	crypto, err := NewRoomCrypto(RoomName)
	if err != nil {
		t.Fatal(err)
	}
	beacon := Beacon{NodeID: "node-ada-1", Name: "ada", NodeName: "workshop", Platform: "linux", Port: 4242,
		TimestampMs: 1750000000000, Control: true}
	nonce := []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
	if got := string(marshal(crypto.encryptWithNonce(BeaconObject(beacon), nonce))); got != beaconEnvelope {
		t.Fatalf("envelope\n got %s\nwant %s", got, beaconEnvelope)
	}
	envelope, _ := parseObject([]byte(beaconEnvelope))
	plain := crypto.Decrypt(envelope)
	parsed, ok := ParseBeacon(plain)
	if !ok || parsed.NodeID != "node-ada-1" || parsed.Port != 4242 || parsed.Name != "ada" || !parsed.Control ||
		parsed.TimestampMs != 1750000000000 || parsed.Reply {
		t.Fatalf("decrypted %v %+v", plain, parsed)
	}
	other, _ := RoomKey("some-other-room", appRoomKey)
	if (&RoomCrypto{key: other}).Decrypt(envelope) != nil {
		t.Fatal("another room's key opens nothing")
	}
	tampered := Object{}
	for key, value := range envelope {
		tampered[key] = value
	}
	tampered["tag"] = "AAAAAAAAAAAAAAAAAAAAAA=="
	if crypto.Decrypt(tampered) != nil {
		t.Fatal("a forged tag is refused")
	}
	tampered["tag"] = envelope["tag"]
	tampered["kind"] = "plain"
	if crypto.Decrypt(tampered) != nil {
		t.Fatal("only a cipher envelope is opened")
	}
	tampered["kind"] = "cipher"
	tampered["nonce"] = "AQID"
	if crypto.Decrypt(tampered) != nil {
		t.Fatal("a short nonce is refused")
	}
	wire := marshal(crypto.Encrypt(BeaconObject(beacon)))
	for _, needle := range []string{"node-ada-1", "workshop", BeaconTag} {
		if strings.Contains(string(wire), needle) {
			t.Fatalf("the beacon datagram carries %q in the clear", needle)
		}
	}
}

func TestRoomSealOpensThePhoneVector(t *testing.T) {
	receiver := mustSeal(t, vectorRoom, urlKey(t, receiverPrivate))
	if receiver.KeyID() != receiverKeyID {
		t.Fatalf("receiver key id %s", receiver.KeyID())
	}
	if !IsSealedFrame(vectorFrame()) || FrameKeyID(vectorFrame()) != senderKeyID {
		t.Fatal("the vector is a sealed frame")
	}
	if got := receiver.Unseal(vectorFrame()); !reflect.DeepEqual(got, vectorPlain()) {
		t.Fatalf("receiver opened %v", got)
	}
	sender := mustSeal(t, vectorRoom, urlKey(t, senderPrivate))
	if sender.KeyID() != senderKeyID || sender.KeyFrame()["pub"] != senderPub {
		t.Fatalf("sender key %s %v", sender.KeyID(), sender.KeyFrame())
	}
	if got := sender.Unseal(vectorFrame()); !reflect.DeepEqual(got, vectorPlain()) {
		t.Fatalf("sender opened %v", got)
	}
	stranger := make([]byte, 32)
	for i := range stranger {
		stranger[i] = byte(i + 1)
	}
	if mustSeal(t, vectorRoom, stranger).Unseal(vectorFrame()) != nil {
		t.Fatal("a stranger opens nothing")
	}
	if mustSeal(t, "chat-channel:42:v1", urlKey(t, receiverPrivate)).Unseal(vectorFrame()) != nil {
		t.Fatal("another room opens nothing")
	}
	if got := mustSeal(t, "  "+vectorRoom+"\n", urlKey(t, receiverPrivate)).Unseal(vectorFrame()); !reflect.DeepEqual(got, vectorPlain()) {
		t.Fatal("the room name is trimmed")
	}
	if got := receiver.Unseal(roundTrip(t, vectorFrame())); !reflect.DeepEqual(got, vectorPlain()) {
		t.Fatal("the vector survives a JSON round trip")
	}
	if receiver.PeerCount() != 1 {
		t.Fatal("a verified sender becomes a peer")
	}
}

func TestRoomSealExchangeAndFrameShapes(t *testing.T) {
	alice := mustSeal(t, vectorRoom, urlKey(t, senderPrivate))
	bob := mustSeal(t, vectorRoom, urlKey(t, receiverPrivate))
	if !IsKeyFrame(alice.KeyFrame()) || !alice.AcceptKeyFrame(roundTrip(t, bob.KeyFrame())) || !bob.AcceptKeyFrame(alice.KeyFrame()) {
		t.Fatal("key frames are exchanged")
	}
	if bob.AcceptKeyFrame(alice.KeyFrame()) || alice.AcceptKeyFrame(alice.KeyFrame()) {
		t.Fatal("a repeated or own advertisement is not new")
	}
	frame := roundTrip(t, alice.Seal(Object{"type": "chat", "text": "no setup required"}))
	if strings.Contains(string(marshal(frame)), "no setup required") {
		t.Fatal("sealed text is not readable")
	}
	keys := frame["keys"].([]any)
	if len(keys) != 2 || str(keys[0].(Object)["kid"]) != receiverKeyID || str(keys[1].(Object)["kid"]) != senderKeyID {
		t.Fatalf("recipients in key-id order: %v", keys)
	}
	if str(bob.Unseal(frame)["text"]) != "no setup required" || str(alice.Unseal(frame)["text"]) != "no setup required" {
		t.Fatal("both recipients open it")
	}
	relabelled := Object{}
	for key, value := range frame {
		relabelled[key] = value
	}
	relabelled["kid"] = receiverKeyID
	if bob.Unseal(relabelled) != nil {
		t.Fatal("a relabelled sender is refused")
	}
	if IsSealedFrame(Object{"type": "chat", "text": "plaintext"}) || IsSealedFrame(Object{"type": "sealed", "v": 2, "kid": "short"}) {
		t.Fatal("not sealed frames")
	}
	relabelled["kid"] = senderKeyID
	relabelled["v"] = 2.0
	if !IsSealedFrame(relabelled) {
		t.Fatal("v 2.0 is v 2")
	}
	relabelled["v"] = 2.5
	if IsSealedFrame(relabelled) {
		t.Fatal("a fractional version is not 2")
	}
	relabelled["v"] = 2
	relabelled["keys"] = []any{}
	if IsSealedFrame(relabelled) || FrameKeyID(Object{"type": "chat"}) != "" {
		t.Fatal("no recipients, no frame")
	}
}

func TestRoomSealRejectsMalformedFrames(t *testing.T) {
	alice := mustSeal(t, vectorRoom, urlKey(t, senderPrivate))
	bob := mustSeal(t, vectorRoom, urlKey(t, receiverPrivate))
	alice.AcceptKeyFrame(bob.KeyFrame())
	frame := roundTrip(t, alice.Seal(Object{"type": "dm", "text": "hi bob"}))
	mutate := func(change func(Object)) Object {
		copy := roundTrip(t, frame)
		change(copy)
		return copy
	}
	wrap := func(change func(Object)) Object {
		return mutate(func(copy Object) {
			for _, entry := range copy["keys"].([]any) {
				change(entry.(Object))
			}
		})
	}
	body := qtBase64(str(frame["body"]))
	body[0] ^= 1
	bad := []Object{
		mutate(func(c Object) { c["nonce"] = "AAAA" }),
		mutate(func(c Object) { c["tag"] = "not base64!" }),
		mutate(func(c Object) { c["body"] = "" }),
		mutate(func(c Object) { c["pub"] = "SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTA" }),
		mutate(func(c Object) { c["body"] = stdBase64(body) }),
		wrap(func(c Object) { c["key"] = stdBase64(make([]byte, 31)) }),
		wrap(func(c Object) { c["tag"] = stdBase64(make([]byte, 16)) }),
		wrap(func(c Object) { c["nonce"] = 12 }),
		mutate(func(c Object) { c["keys"] = []any{"not a map"} }),
	}
	for index, candidate := range bad {
		if bob.Unseal(candidate) != nil {
			t.Fatalf("case %d opened", index)
		}
	}
	if str(bob.Unseal(frame)["text"]) != "hi bob" || bob.PeerCount() != 1 {
		t.Fatal("the intact frame opens and its sender is remembered")
	}
}

func TestRoomSealPeerListIsCapped(t *testing.T) {
	seal := mustSeal(t, vectorRoom, urlKey(t, receiverPrivate))
	var persisted []PeerKey
	seal.OnPersist = func(peers []PeerKey) { persisted = peers }
	peers := make([]*RoomSeal, 70)
	for i := range peers {
		peers[i] = mustSeal(t, vectorRoom, randomBytes(32))
		if !seal.AcceptKeyFrame(peers[i].KeyFrame()) {
			t.Fatalf("peer %d is new", i)
		}
	}
	if seal.PeerCount() != 63 || len(persisted) != 63 {
		t.Fatalf("peer count %d persisted %d", seal.PeerCount(), len(persisted))
	}
	frame := seal.Seal(Object{"type": "chat", "text": "to many"})
	if len(frame["keys"].([]any)) != 64 {
		t.Fatal("63 peers plus this device")
	}
	if peers[0].Unseal(roundTrip(t, frame)) != nil || str(peers[69].Unseal(roundTrip(t, frame))["text"]) != "to many" {
		t.Fatal("the oldest peer was evicted")
	}
	restored := mustSeal(t, vectorRoom, urlKey(t, receiverPrivate))
	restored.LoadPeers(append([]PeerKey{{KeyID: "bogus", Public: "x"}}, persisted...))
	if restored.PeerCount() != 63 {
		t.Fatalf("restored %d", restored.PeerCount())
	}
	raw := marshal([]PeerKey{{KeyID: senderKeyID, Public: senderPub}})
	if string(raw) != `[{"kid":"`+senderKeyID+`","pub":"`+senderPub+`"}]` {
		t.Fatalf("persisted shape %s", raw)
	}
	var back []PeerKey
	if json.Unmarshal(raw, &back) != nil || parsePeerKeys(raw)[0] != back[0] {
		t.Fatal("persisted peers parse back")
	}
}

func TestSealPeerSettingKeyMatchesQt(t *testing.T) {
	if got := SealPeersSettingKey("ab+/=c"); got != "chat/e2ee/peersV2/forkmesh-lan-mesh-v1%0Aab%2B%2F%3Dc" {
		t.Fatalf("setting key %s", got)
	}
}
