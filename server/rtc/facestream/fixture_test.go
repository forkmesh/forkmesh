package facestream

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

const fixtureName = "face-stream-parity.json"

var fixtureCopies = []string{
	"../../../native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/" + fixtureName,
	"../../../native/android/core/src/test/resources/chat/" + fixtureName,
}

type fixtureFrame struct {
	Key        bool      `json:"key"`
	Stale      bool      `json:"stale"`
	Seq        int       `json:"seq"`
	KeyID      int       `json:"keyId"`
	Time       uint32    `json:"time"`
	Width      int       `json:"width"`
	Height     int       `json:"height"`
	Precision  int       `json:"precision"`
	Pose       []float64 `json:"pose"`
	Expression []float64 `json:"expression"`
	Points     []float64 `json:"points"`
}

type fixtureInput struct {
	Time       uint32    `json:"time"`
	Points     []float64 `json:"points"`
	Pose       []float64 `json:"pose"`
	Expression []float64 `json:"expression"`
}

type fixtureDecoded struct {
	Index int           `json:"index"`
	Error string        `json:"error,omitempty"`
	Frame *fixtureFrame `json:"frame,omitempty"`
}

type fixtureStream struct {
	Name         string           `json:"name"`
	Config       Config           `json:"config"`
	RequestKeyAt []int            `json:"requestKeyAt"`
	DecodeFrom   int              `json:"decodeFrom"`
	Drop         []int            `json:"drop"`
	Inputs       []fixtureInput   `json:"inputs"`
	Packets      []string         `json:"packets"`
	Decoded      []fixtureDecoded `json:"decoded"`
}

type fixtureReference struct {
	ID     int       `json:"id"`
	Format int       `json:"format"`
	Time   uint32    `json:"time"`
	Crop   []int     `json:"crop"`
	Width  int       `json:"width"`
	Height int       `json:"height"`
	Points []float64 `json:"points"`
	MaskW  int       `json:"maskW"`
	MaskH  int       `json:"maskH"`
	Mask   string    `json:"mask"`
	Image  string    `json:"image"`
}

type fixtureReferenceCase struct {
	Input   fixtureReference `json:"input"`
	Packet  string           `json:"packet"`
	Decoded fixtureReference `json:"decoded"`
}

type fixtureTile struct {
	State  int       `json:"state"`
	Mouth  float64   `json:"mouth"`
	Smile  float64   `json:"smile"`
	Points []float64 `json:"points"`
}

type fixtureAtlas struct {
	ID     int           `json:"id"`
	Time   uint32        `json:"time"`
	TileW  int           `json:"tileW"`
	TileH  int           `json:"tileH"`
	Format int           `json:"format"`
	Tiles  []fixtureTile `json:"tiles"`
	Image  string        `json:"image"`
}

type fixtureAtlasCase struct {
	Input   fixtureAtlas `json:"input"`
	Packet  string       `json:"packet"`
	Decoded fixtureAtlas `json:"decoded"`
}

type fixtureFile struct {
	Format     string                 `json:"format"`
	Version    int                    `json:"version"`
	Template   [][]float64            `json:"template"`
	Streams    []fixtureStream        `json:"streams"`
	References []fixtureReferenceCase `json:"references"`
	Atlases    []fixtureAtlasCase     `json:"atlases"`
}

func flatPoints(p [LandmarkCount][2]float64) []float64 {
	out := make([]float64, 0, LandmarkCount*2)
	for _, q := range p {
		out = append(out, q[0], q[1])
	}
	return out
}

func poseValues(p Pose) []float64 {
	return []float64{p.Rotation.W, p.Rotation.X, p.Rotation.Y, p.Rotation.Z, p.X, p.Y, p.Scale}
}

func expressionList(x Expression) []float64 {
	return []float64{x.LidLeft, x.LidRight, x.GazeX, x.GazeY, x.Mouth, x.Smile, x.Tongue}
}

func toFixtureFrame(f Frame) fixtureFrame {
	return fixtureFrame{f.Key, f.Stale, f.Seq, f.KeyID, f.Time, f.Width, f.Height, f.Precision, poseValues(f.Pose), expressionList(f.Expression), flatPoints(f.Points)}
}

func errorName(err error) string {
	switch {
	case errors.Is(err, ErrNeedKey):
		return "need-key"
	case errors.Is(err, ErrOld):
		return "old"
	case errors.Is(err, ErrShort):
		return "short"
	case errors.Is(err, ErrCorrupt):
		return "corrupt"
	default:
		return err.Error()
	}
}

func contains(list []int, v int) bool {
	for _, x := range list {
		if x == v {
			return true
		}
	}
	return false
}

func buildStream(t *testing.T, name string, cfg Config, opts synthOptions, requestKeyAt []int, jumpAt int, decodeFrom int, drop []int) fixtureStream {
	inputs, _ := synthSequence(opts)
	for i := jumpAt; jumpAt > 0 && i < len(inputs); i++ {
		inputs[i].Time += 6000
	}
	enc, err := NewEncoder(cfg)
	if err != nil {
		t.Fatal(err)
	}
	s := fixtureStream{Name: name, Config: enc.Config(), RequestKeyAt: requestKeyAt, DecodeFrom: decodeFrom, Drop: drop}
	dec := NewDecoder()
	for i := range inputs {
		in := &inputs[i]
		s.Inputs = append(s.Inputs, fixtureInput{in.Time, flatPoints(in.Points), poseValues(in.Pose), expressionList(in.Expression)})
		if contains(requestKeyAt, i) {
			enc.RequestKey()
		}
		p, err := enc.Encode(in)
		if err != nil {
			t.Fatal(err)
		}
		s.Packets = append(s.Packets, hex.EncodeToString(p))
		if i < decodeFrom || contains(drop, i) {
			continue
		}
		got, err := dec.Decode(p)
		if err != nil {
			s.Decoded = append(s.Decoded, fixtureDecoded{Index: i, Error: errorName(err)})
			continue
		}
		f := toFixtureFrame(*got)
		s.Decoded = append(s.Decoded, fixtureDecoded{Index: i, Frame: &f})
	}
	return s
}

func toFixtureReference(r *Reference) fixtureReference {
	var mask []byte
	for _, v := range r.Mask {
		mask = append(mask, '0'+v)
	}
	return fixtureReference{r.ID, r.Format, r.Time, []int{r.CropX, r.CropY, r.CropW, r.CropH}, r.Width, r.Height, flatPoints(r.Points),
		r.MaskW, r.MaskH, string(mask), hex.EncodeToString(r.Image)}
}

func toFixtureAtlas(a *Atlas) fixtureAtlas {
	out := fixtureAtlas{a.ID, a.Time, a.TileW, a.TileH, a.Format, nil, hex.EncodeToString(a.Image)}
	for _, t := range a.Tiles {
		points := make([]float64, 0, MouthPointCount*2)
		for _, p := range t.Points {
			points = append(points, p[0], p[1])
		}
		out.Tiles = append(out.Tiles, fixtureTile{t.State, t.Mouth, t.Smile, points})
	}
	return out
}

func buildFixture(t *testing.T) []byte {
	f := fixtureFile{Format: "face-stream-parity", Version: Version}
	for _, p := range Template {
		f.Template = append(f.Template, []float64{p[0], p[1], p[2]})
	}
	f.Streams = append(f.Streams,
		buildStream(t, "talking", Config{Width: 1280, Height: 720}, synthOptions{frames: 75, fps: 30, noise: 0.5, seed: 7, jitter: true}, []int{40}, 0, 0, []int{50, 51}),
		buildStream(t, "small", Config{Width: 640, Height: 480, FPS: 24, Precision: 48, KeyInterval: 12}, synthOptions{frames: 40, fps: 24, noise: 0.8, seed: 21}, nil, 20, 3, []int{30}),
		buildStream(t, "still", Config{Width: 1280, Height: 720}, synthOptions{frames: 40, fps: 30, seed: 5, still: true}, []int{}, 0, 0, []int{}),
	)
	plain := referenceSample()
	plain.ID, plain.Format, plain.MaskW, plain.MaskH, plain.Mask = 15, FormatHEIC, 0, 0, nil
	for _, r := range []*Reference{referenceSample(), plain} {
		packet, err := EncodeReference(r)
		if err != nil {
			t.Fatal(err)
		}
		got, err := DecodeReference(packet)
		if err != nil {
			t.Fatal(err)
		}
		f.References = append(f.References, fixtureReferenceCase{toFixtureReference(r), hex.EncodeToString(packet), toFixtureReference(got)})
	}
	single := atlasSample()
	single.ID, single.Format, single.TileW, single.TileH, single.Tiles = 15, FormatWebP, 32, 20, single.Tiles[1:2]
	for _, a := range []*Atlas{atlasSample(), single} {
		packet, err := EncodeAtlas(a)
		if err != nil {
			t.Fatal(err)
		}
		got, err := DecodeAtlas(packet)
		if err != nil {
			t.Fatal(err)
		}
		f.Atlases = append(f.Atlases, fixtureAtlasCase{toFixtureAtlas(a), hex.EncodeToString(packet), toFixtureAtlas(got)})
	}
	out, err := json.Marshal(f)
	if err != nil {
		t.Fatal(err)
	}
	return append(out, '\n')
}

func TestParityFixtureIsCurrent(t *testing.T) {
	want := buildFixture(t)
	path := filepath.Join("testdata", fixtureName)
	if os.Getenv("FACESTREAM_WRITE_FIXTURE") == "1" {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		for _, p := range append([]string{path}, fixtureCopies...) {
			if err := os.WriteFile(p, want, 0o644); err != nil {
				t.Fatal(err)
			}
		}
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%v (regenerate with FACESTREAM_WRITE_FIXTURE=1)", err)
	}
	if !bytes.Equal(got, want) {
		t.Fatal("fixture is stale: regenerate with FACESTREAM_WRITE_FIXTURE=1 go test ./rtc/facestream/")
	}
	for _, p := range fixtureCopies {
		copyBytes, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		if !bytes.Equal(copyBytes, want) {
			t.Fatalf("%s differs from testdata/%s", p, fixtureName)
		}
	}
}
