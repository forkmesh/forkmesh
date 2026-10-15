package mesh

import (
	"net"
	"strings"
	"testing"
)

func TestCrossClientConstants(t *testing.T) {
	if Conversation != "#mesh" || RoomName != "forkmesh-lan-mesh-v1" || MulticastGroupV4 != "239.192.79.42" ||
		DefaultDiscoveryPort != 48732 || BluetoothServiceUUID != "7f9e5c10-4d3b-4a21-9c6f-2b7ad1e05f11" ||
		MaxFrameBytes != 24*1024*1024 {
		t.Fatal("the constants every client hard-codes changed")
	}
}

func TestAddressPolicy(t *testing.T) {
	for _, text := range []string{"192.168.1.20", "10.4.4.4", "172.20.0.9", "169.254.7.7", "127.0.0.1", "::1", "fd12::1", "fe80::1", "::ffff:192.168.5.5"} {
		if !IsLocalNetworkAddress(net.ParseIP(text)) {
			t.Fatalf("%s is local", text)
		}
	}
	for _, text := range []string{"8.8.8.8", "172.32.0.1", "100.64.0.1", "2606:4700::1", "::ffff:8.8.8.8"} {
		if IsLocalNetworkAddress(net.ParseIP(text)) {
			t.Fatalf("%s is not local", text)
		}
	}
	if IsLocalNetworkAddress(nil) {
		t.Fatal("no address is not local")
	}
}

func TestSweepAddresses(t *testing.T) {
	home := SweepAddresses(net.ParseIP("192.168.1.228"), 24)
	if len(home) != 253 || home[0].String() != "192.168.1.1" || home[252].String() != "192.168.1.254" {
		t.Fatalf("a /24 sweep %d", len(home))
	}
	for _, host := range home {
		if host.String() == "192.168.1.228" {
			t.Fatal("the sweep skips this host")
		}
	}
	if len(SweepAddresses(net.ParseIP("10.4.0.9"), 8)) != 253 || len(SweepAddresses(net.ParseIP("10.4.0.9"), -1)) != 253 {
		t.Fatal("a wide or unknown prefix sweeps the /24")
	}
	if got := SweepAddresses(net.ParseIP("192.168.1.9"), 30); len(got) != 1 || got[0].String() != "192.168.1.10" {
		t.Fatalf("a /30 sweep %v", got)
	}
	hotspot := SweepAddresses(net.ParseIP("172.20.10.1"), 28)
	if len(hotspot) != 13 || hotspot[0].String() != "172.20.10.2" || hotspot[12].String() != "172.20.10.14" {
		t.Fatalf("a tether /28 %v", hotspot)
	}
	for _, text := range []string{"8.8.8.8", "127.0.0.1", "169.254.3.4", "fe80::1"} {
		if len(SweepAddresses(net.ParseIP(text), 24)) != 0 {
			t.Fatalf("%s is not swept", text)
		}
	}
	if len(SweepAddresses(net.ParseIP("192.168.1.9"), 32)) != 0 || len(SweepAddresses(net.ParseIP("192.168.1.9"), 31)) != 0 {
		t.Fatal("point-to-point is not swept")
	}
}

func TestFraming(t *testing.T) {
	payload := []byte(`{"hello":"mesh"}`)
	stream := append(FrameBytes(payload), FrameBytes(payload)...)
	for i := 0; i < 2; i++ {
		got, status := TakeFrame(&stream)
		if status != 1 || string(got) != string(payload) {
			t.Fatalf("frame %d: %q %d", i, got, status)
		}
	}
	if _, status := TakeFrame(&stream); status != 0 || len(stream) != 0 {
		t.Fatal("an empty buffer yields nothing")
	}
	partial := FrameBytes(payload)
	partial = partial[:len(partial)-3]
	if _, status := TakeFrame(&partial); status != 0 || len(partial) != len(payload)+1 {
		t.Fatal("a half frame waits")
	}
	huge := []byte{0x01, 0x80, 0x00, 0x01}
	if _, status := TakeFrame(&huge); status != -1 {
		t.Fatal("an oversized frame is refused")
	}
	zero := []byte{0, 0, 0, 0}
	if _, status := TakeFrame(&zero); status != -1 {
		t.Fatal("a zero-length frame is refused")
	}
	if got := FrameBytes([]byte("ab")); string(got) != "\x00\x00\x00\x02ab" {
		t.Fatalf("big-endian length %q", got)
	}
}

func TestBeaconParsing(t *testing.T) {
	beacon := Beacon{NodeID: "node-ada-1", Name: "ada", NodeName: "workshop", OwnerUser: "ada", Platform: "linux",
		Version: "9.9.9", Port: 4242, TimestampMs: 1750000000000}
	parsed, ok := ParseBeacon(roundTripPlain(t, BeaconObject(beacon)))
	if !ok || parsed != beacon {
		t.Fatalf("round trip %+v", parsed)
	}
	for _, change := range []func(Object){
		func(o Object) { o["t"] = "something-else" },
		func(o Object) { o["v"] = 99 },
		func(o Object) { o["port"] = 0 },
		func(o Object) { o["port"] = 70000 },
		func(o Object) { o["id"] = "   " },
	} {
		object := roundTripPlain(t, BeaconObject(beacon))
		change(object)
		if _, ok := ParseBeacon(object); ok {
			t.Fatalf("parsed %v", object)
		}
	}
	if _, ok := ParseBeacon(Object{}); ok {
		t.Fatal("empty")
	}
	long := roundTripPlain(t, BeaconObject(beacon))
	long["name"] = strings.Repeat("x", 50)
	long["platform"] = strings.Repeat("p", 40)
	long["id"] = "  " + strings.Repeat("i", 200) + " "
	got, _ := ParseBeacon(long)
	if len(got.Name) != 32 || len(got.Platform) != 16 || len(got.NodeID) != 160 {
		t.Fatalf("bounded %d %d %d", len(got.Name), len(got.Platform), len(got.NodeID))
	}
	if left16("ab😀cd", 3) != "ab" || left16("ab😀cd", 4) != "ab😀" || len16("😀") != 2 {
		t.Fatal("lengths count UTF-16 units like QString")
	}
}

func roundTripPlain(t *testing.T, object Object) Object {
	t.Helper()
	parsed, ok := parseObject(marshal(object))
	if !ok {
		t.Fatal("not an object")
	}
	return parsed
}

func TestWakeTokens(t *testing.T) {
	if WakeTokenExpiry("mw1.abc.42.sig") != 42 || WakeTokenExpiry("mw2.abc.42.sig") != 0 || WakeTokenExpiry("mw1.abc.42") != 0 ||
		WakeTokenExpiry("mw1.abc.soon.sig") != 0 || WakeTokenExpiry("mw1."+strings.Repeat("a", 200)+".42.sig") != 0 {
		t.Fatal("a wake token is read only in its own shape")
	}
}

func TestControlHelloAndFileNames(t *testing.T) {
	if !IsControlHello([]byte(`{"t":"fm-control-hello","v":1}`)) || IsControlHello([]byte(`{"t":"other","x":"fm-control-hello"}`)) ||
		IsControlHello([]byte(`{"kind":"cipher"}`)) {
		t.Fatal("control hello detection")
	}
	if SafeFileName(`C:\Users\ada\notes.txt`) != "notes.txt" || SafeFileName("dir/ ") != "file" || SafeFileName("a\x00b") != "ab" ||
		len(SafeFileName(strings.Repeat("n", 300))) != 180 {
		t.Fatal("file names")
	}
	if string(qtBase64("aGVs\nbG8=")) != "hello" || string(qtBase64("aGVsbG8")) != "hello" {
		t.Fatal("lenient base64")
	}
}
