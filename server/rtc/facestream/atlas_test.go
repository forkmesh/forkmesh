package facestream

import (
	"bytes"
	"errors"
	"math"
	"testing"
)

func atlasSample() *Atlas {
	a := &Atlas{ID: 5, Time: 987654, TileW: 64, TileH: 48, Format: FormatJPEG, Image: bytes.Repeat([]byte{0xFF, 0xD8, 0x2A}, 400)}
	inputs, _ := synthSequence(synthOptions{frames: 1, fps: 30, seed: 9})
	p := inputs[0].Points
	cx := (p[48][0] + p[54][0]) / 2
	cy := (p[48][1] + p[54][1]) / 2
	scale := float64(a.TileW) / (1.6 * math.Hypot(p[54][0]-p[48][0], p[54][1]-p[48][1]))
	for state := MouthClosed; state <= MouthRound; state++ {
		t := AtlasTile{State: state, Mouth: []float64{0.02, 0.61, 0.3, 0.27}[state], Smile: []float64{0.4, 0.33, 0.9, 0.05}[state]}
		for i := range t.Points {
			q := p[48+i]
			open := 0.0
			if (i >= 7 && i <= 11) || i >= 17 {
				open = t.Mouth * 6
			}
			t.Points[i] = [2]float64{(q[0]-cx)*scale + float64(a.TileW)/2, (q[1]-cy)*scale + float64(a.TileH)/2 + open}
		}
		a.Tiles = append(a.Tiles, t)
	}
	return a
}

func TestAtlasRoundTrip(t *testing.T) {
	a := atlasSample()
	packet, err := EncodeAtlas(a)
	if err != nil {
		t.Fatal(err)
	}
	if got := len(packet) - len(a.Image); got != atlasFixed+4*atlasTileBytes {
		t.Fatalf("metadata %d bytes", got)
	}
	if PacketKind(packet) != KindAtlas {
		t.Fatal("kind")
	}
	got, err := DecodeAtlas(packet)
	if err != nil {
		t.Fatal(err)
	}
	if got.ID != 5 || got.Time != 987654 || got.TileW != 64 || got.TileH != 48 || len(got.Tiles) != 4 || !bytes.Equal(got.Image, a.Image) {
		t.Fatalf("%+v", got)
	}
	for k, tile := range got.Tiles {
		want := a.Tiles[k]
		if tile.State != want.State || math.Abs(tile.Mouth-want.Mouth) > 1.0/32 || math.Abs(tile.Smile-want.Smile) > 1.0/32 {
			t.Fatalf("tile %d: %+v", k, tile)
		}
		for i, q := range tile.Points {
			for axis := 0; axis < 2; axis++ {
				if math.Abs(q[axis]-want.Points[i][axis]) > 1.0/32 {
					t.Fatalf("tile %d point %d", k, i)
				}
			}
		}
	}
	if _, err := NewDecoder().Decode(packet[:64]); !errors.Is(err, ErrNotAFrame) {
		t.Fatalf("frame decoder: %v", err)
	}
	if _, err := DecodeReference(packet); !errors.Is(err, ErrNotPicture) {
		t.Fatalf("reference decoder: %v", err)
	}
}

func TestAtlasRefusesBadInput(t *testing.T) {
	for name, edit := range map[string]func(*Atlas){
		"state":  func(a *Atlas) { a.Tiles[0].State = 4 },
		"side":   func(a *Atlas) { a.TileW = 7 },
		"tall":   func(a *Atlas) { a.TileH = 129 },
		"none":   func(a *Atlas) { a.Tiles = nil },
		"many":   func(a *Atlas) { a.Tiles = append(a.Tiles, a.Tiles...); a.Tiles = append(a.Tiles, a.Tiles[0]) },
		"image":  func(a *Atlas) { a.Image = nil },
		"format": func(a *Atlas) { a.Format = 3 },
		"nan":    func(a *Atlas) { a.Tiles[1].Points[3][0] = math.NaN() },
		"big":    func(a *Atlas) { a.Image = make([]byte, MaxAtlasPacket) },
	} {
		a := atlasSample()
		edit(a)
		if _, err := EncodeAtlas(a); err == nil {
			t.Fatalf("%s accepted", name)
		}
	}
	packet, _ := EncodeAtlas(atlasSample())
	bad := append([]byte(nil), packet...)
	bad[7] = 9
	if _, err := DecodeAtlas(bad); !errors.Is(err, ErrCorrupt) {
		t.Fatalf("count: %v", err)
	}
	bad = append([]byte(nil), packet...)
	bad[atlasFixed+1] = 17
	if _, err := DecodeAtlas(bad); !errors.Is(err, ErrCorrupt) {
		t.Fatalf("level: %v", err)
	}
	if _, err := DecodeAtlas(packet[:atlasFixed+4*atlasTileBytes]); !errors.Is(err, ErrCorrupt) {
		t.Fatalf("no image: %v", err)
	}
	if _, err := DecodeAtlas(packet[:20]); !errors.Is(err, ErrShort) {
		t.Fatalf("short: %v", err)
	}
}

func FuzzDecodeAtlas(f *testing.F) {
	packet, _ := EncodeAtlas(atlasSample())
	f.Add(packet)
	f.Add(packet[:atlasFixed+atlasTileBytes+1])
	f.Fuzz(func(t *testing.T, data []byte) {
		a, err := DecodeAtlas(data)
		if err != nil {
			return
		}
		if len(a.Tiles) < 1 || len(a.Tiles) > MaxAtlasTiles || len(a.Image) == 0 || a.TileW > MaxAtlasSide || a.TileH > MaxAtlasSide {
			t.Fatal("bounds")
		}
		for _, tile := range a.Tiles {
			if tile.Mouth > 1 || tile.Smile > 1 || tile.State > MouthRound {
				t.Fatal("tile bounds")
			}
		}
	})
}
