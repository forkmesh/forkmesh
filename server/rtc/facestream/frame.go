package facestream

import (
	"encoding/binary"
	"errors"
	"math"
)

const (
	Version            = 1
	KindDelta          = 0
	KindKey            = 1
	KindReference      = 2
	DefaultPrecision   = 96
	DefaultKeyInterval = 30
	DefaultFPS         = 30
	MaxFramePacket     = 1024
	MinPrecision       = 16
	MaxPrecision       = 255
	MaxSide            = 4096
	keyHeader          = 12
	quatSteps          = 512
	transSteps         = 2
	scaleSteps         = 8
	maxTimeJump        = 4000
	expressionCount    = 7
)

var (
	ErrShort      = errors.New("facestream: packet too short")
	ErrLong       = errors.New("facestream: packet too long")
	ErrVersion    = errors.New("facestream: unknown version")
	ErrKind       = errors.New("facestream: unknown kind")
	ErrHeader     = errors.New("facestream: bad header")
	ErrNeedKey    = errors.New("facestream: waiting for a key frame")
	ErrOld        = errors.New("facestream: old or repeated frame")
	ErrInput      = errors.New("facestream: input is not finite")
	ErrConfig     = errors.New("facestream: bad config")
	ErrCorrupt    = errors.New("facestream: corrupt payload")
	ErrNotAFrame  = errors.New("facestream: not a frame packet")
	ErrNotPicture = errors.New("facestream: not a reference packet")
)

type Quat struct{ W, X, Y, Z float64 }

type Pose struct {
	Rotation Quat
	X, Y     float64
	Scale    float64
}

type Expression struct {
	LidLeft, LidRight float64
	GazeX, GazeY      float64
	Mouth, Smile      float64
	Tongue            float64
}

type Input struct {
	Time       uint32
	Points     [LandmarkCount][2]float64
	Pose       Pose
	Expression Expression
}

type Frame struct {
	Key        bool
	Stale      bool
	Seq        int
	KeyID      int
	Time       uint32
	Width      int
	Height     int
	Precision  int
	Pose       Pose
	Expression Expression
	Points     [LandmarkCount][2]float64
}

type Config struct {
	Width       int `json:"width"`
	Height      int `json:"height"`
	FPS         int `json:"fps"`
	Precision   int `json:"precision"`
	KeyInterval int `json:"keyInterval"`
}

type expressionChannel struct {
	steps  float64
	low    int
	high   int
	signed bool
	prior  prob
}

var expressionChannels = [expressionCount]expressionChannel{
	{16, 0, 16, false, 3600},
	{16, 0, 16, false, 3600},
	{64, -32, 32, true, probHalf},
	{64, -32, 32, true, probHalf},
	{16, 0, 16, false, probHalf},
	{16, 0, 16, false, 2800},
	{16, 0, 16, false, 3900},
}

type qpose struct{ qx, qy, qz, tx, ty, s int }

type rows [3][3]float64

func m(a, b float64) float64 { return float64(a * b) }

func floorHalf(v float64) int {
	f := math.Floor(v + 0.5)
	if f > 1e9 {
		return 1e9
	}
	if f < -1e9 {
		return -1e9
	}
	return int(f)
}

func finite(v float64) bool { return !math.IsNaN(v) && !math.IsInf(v, 0) }

func quantizePose(p Pose) qpose {
	q := p.Rotation
	n := math.Sqrt(m(q.W, q.W) + m(q.X, q.X) + m(q.Y, q.Y) + m(q.Z, q.Z))
	x, y, z := 0.0, 0.0, 0.0
	if n > 0 {
		x, y, z = q.X/n, q.Y/n, q.Z/n
		if q.W < 0 {
			x, y, z = -x, -y, -z
		}
	}
	return qpose{
		qx: clampInt(floorHalf(x*quatSteps), -quatSteps, quatSteps),
		qy: clampInt(floorHalf(y*quatSteps), -quatSteps, quatSteps),
		qz: clampInt(floorHalf(z*quatSteps), -quatSteps, quatSteps),
		tx: clampInt(floorHalf(p.X*transSteps), -32768, 32767),
		ty: clampInt(floorHalf(p.Y*transSteps), -32768, 32767),
		s:  clampInt(floorHalf(p.Scale*scaleSteps), 1, 32767),
	}
}

func (p qpose) quat() Quat {
	x := float64(p.qx) / quatSteps
	y := float64(p.qy) / quatSteps
	z := float64(p.qz) / quatSteps
	ss := m(x, x) + m(y, y) + m(z, z)
	w := 0.0
	if ss < 1 {
		w = math.Sqrt(1 - ss)
	}
	n := math.Sqrt(m(w, w) + m(x, x) + m(y, y) + m(z, z))
	return Quat{w / n, x / n, y / n, z / n}
}

func (p qpose) pose() Pose {
	return Pose{Rotation: p.quat(), X: float64(p.tx) / transSteps, Y: float64(p.ty) / transSteps, Scale: float64(p.s) / scaleSteps}
}

func RotationRows(q Quat) [3][3]float64 {
	w, x, y, z := q.W, q.X, q.Y, q.Z
	return [3][3]float64{
		{m(w, w) + m(x, x) - m(y, y) - m(z, z), 2 * (m(x, y) - m(w, z)), 2 * (m(x, z) + m(w, y))},
		{2 * (m(x, y) + m(w, z)), m(w, w) - m(x, x) + m(y, y) - m(z, z), 2 * (m(y, z) - m(w, x))},
		{2 * (m(x, z) - m(w, y)), 2 * (m(y, z) + m(w, x)), m(w, w) - m(x, x) - m(y, y) + m(z, z)},
	}
}

func dot3(r [3]float64, v [3]float64) float64 {
	return m(r[0], v[0]) + m(r[1], v[1]) + m(r[2], v[2])
}

func Project(p Pose) [LandmarkCount][2]float64 {
	r := RotationRows(p.Rotation)
	var out [LandmarkCount][2]float64
	for i := range Template {
		out[i][0] = p.X + m(p.Scale, dot3(r[0], Template[i]))
		out[i][1] = p.Y + m(p.Scale, dot3(r[1], Template[i]))
	}
	return out
}

func quantizeExpression(x Expression) [expressionCount]int {
	values := [expressionCount]float64{x.LidLeft, x.LidRight, x.GazeX, x.GazeY, x.Mouth, x.Smile, x.Tongue}
	var out [expressionCount]int
	for i, c := range expressionChannels {
		out[i] = clampInt(floorHalf(values[i]*c.steps), c.low, c.high)
	}
	return out
}

func expressionValues(q [expressionCount]int) Expression {
	v := func(i int) float64 { return float64(q[i]) / expressionChannels[i].steps }
	return Expression{v(0), v(1), v(2), v(3), v(4), v(5), v(6)}
}

type landmarkModels [groupCount * 2]intModel

func freshLandmarkModels() landmarkModels {
	var out landmarkModels
	for i := range out {
		out[i] = newIntModel(probHalf)
	}
	return out
}

type poseModels [6]intModel

func freshPoseModels() poseModels {
	var out poseModels
	for i := range out {
		out[i] = newIntModel(probHalf)
	}
	return out
}

func freshExpressionModels() [expressionCount]intModel {
	var out [expressionCount]intModel
	for i, c := range expressionChannels {
		out[i] = newIntModel(c.prior)
	}
	return out
}

func nominalInterval(fps int) int { return (1000 + fps/2) / fps }

func (c Config) normalized() (Config, error) {
	if c.FPS == 0 {
		c.FPS = DefaultFPS
	}
	if c.Precision == 0 {
		c.Precision = DefaultPrecision
	}
	if c.KeyInterval == 0 {
		c.KeyInterval = DefaultKeyInterval
	}
	if c.Width < 1 || c.Width > MaxSide || c.Height < 1 || c.Height > MaxSide || c.FPS < 1 || c.FPS > 120 ||
		c.Precision < MinPrecision || c.Precision > MaxPrecision || c.KeyInterval < 1 || c.KeyInterval > 255 {
		return c, ErrConfig
	}
	return c, nil
}

type state struct {
	cfg       Config
	keyID     int
	keyPose   qpose
	seq       int
	time      uint32
	chain     landmarkModels
	recon     [LandmarkCount][2]float64
	projected [LandmarkCount][2]float64
}

type Encoder struct {
	s        state
	started  bool
	sinceKey int
	forceKey bool
	last     Frame
}

func NewEncoder(cfg Config) (*Encoder, error) {
	c, err := cfg.normalized()
	if err != nil {
		return nil, err
	}
	return &Encoder{s: state{cfg: c, seq: 255, keyID: 15}}, nil
}

func (e *Encoder) RequestKey() { e.forceKey = true }

func (e *Encoder) Last() Frame { return e.last }

func (e *Encoder) Config() Config { return e.s.cfg }

func validInput(in *Input) bool {
	for _, p := range in.Points {
		if !finite(p[0]) || !finite(p[1]) {
			return false
		}
	}
	x := in.Expression
	for _, v := range []float64{in.Pose.Rotation.W, in.Pose.Rotation.X, in.Pose.Rotation.Y, in.Pose.Rotation.Z, in.Pose.X, in.Pose.Y, in.Pose.Scale,
		x.LidLeft, x.LidRight, x.GazeX, x.GazeY, x.Mouth, x.Smile, x.Tongue} {
		if !finite(v) {
			return false
		}
	}
	return true
}

func header(kind, id int) byte { return byte(kind<<6 | Version<<4 | id&15) }

func (e *Encoder) Encode(in *Input) ([]byte, error) {
	if !validInput(in) {
		return nil, ErrInput
	}
	s := &e.s
	cfg := s.cfg
	nominal := nominalInterval(cfg.FPS)
	dt := int(int32(in.Time - s.time))
	key := !e.started || e.forceKey || e.sinceKey >= cfg.KeyInterval || dt-nominal > maxTimeJump || dt-nominal < -maxTimeJump
	qp := quantizePose(in.Pose)
	pose := qp.pose()
	projected := Project(pose)
	seq := (s.seq + 1) & 255
	keyID := s.keyID
	if key {
		keyID = (keyID + 1) & 15
	}
	out := make([]byte, 0, 96)
	if key {
		out = append(out, header(KindKey, keyID), byte(seq))
		out = binary.BigEndian.AppendUint16(out, uint16(cfg.Width))
		out = binary.BigEndian.AppendUint16(out, uint16(cfg.Height))
		out = append(out, byte(cfg.FPS), byte(cfg.Precision))
		out = binary.BigEndian.AppendUint32(out, in.Time)
	} else {
		out = append(out, header(KindDelta, keyID), byte(seq))
	}
	enc := newRangeEncoder(out)
	values := [6]int{qp.qx, qp.qy, qp.qz, qp.tx, qp.ty, qp.s}
	if key {
		enc.direct(uint32(qp.qx+quatSteps), 11)
		enc.direct(uint32(qp.qy+quatSteps), 11)
		enc.direct(uint32(qp.qz+quatSteps), 11)
		enc.direct(uint32(qp.tx+32768), 16)
		enc.direct(uint32(qp.ty+32768), 16)
		enc.direct(uint32(qp.s), 15)
	} else {
		timeModel := newIntModel(probHalf)
		enc.int(&timeModel, 0, dt-nominal)
		base := [6]int{s.keyPose.qx, s.keyPose.qy, s.keyPose.qz, s.keyPose.tx, s.keyPose.ty, s.keyPose.s}
		models := freshPoseModels()
		for i := range values {
			enc.int(&models[i], 0, values[i]-base[i])
		}
	}
	qx := quantizeExpression(in.Expression)
	exprModels := freshExpressionModels()
	for i, c := range expressionChannels {
		if c.signed {
			enc.int(&exprModels[i], 0, qx[i])
		} else {
			enc.uint(&exprModels[i], 0, qx[i])
		}
	}
	var models *landmarkModels
	keyModels := freshLandmarkModels()
	if key {
		s.chain = freshLandmarkModels()
		models = &keyModels
	} else {
		models = &s.chain
	}
	step := pose.Scale / float64(cfg.Precision)
	var recon [LandmarkCount][2]float64
	var nonzero [groupCount * 2]int
	for i := 0; i < LandmarkCount; i++ {
		for axis := 0; axis < 2; axis++ {
			var pred float64
			if key {
				pred = projected[i][axis]
			} else {
				pred = s.recon[i][axis] + (projected[i][axis] - s.projected[i][axis])
			}
			r := clampInt(floorHalf((in.Points[i][axis]-pred)/step), -maxInt, maxInt)
			class := landmarkGroup(i)*2 + axis
			enc.int(&models[class], nonzero[class], r)
			if r != 0 {
				nonzero[class] = 1
			} else {
				nonzero[class] = 0
			}
			recon[i][axis] = pred + m(float64(r), step)
		}
	}
	packet := enc.finish()
	if key {
		s.keyID = keyID
		s.keyPose = qp
		e.sinceKey = 0
		e.forceKey = false
	}
	e.started = true
	e.sinceKey++
	s.seq = seq
	s.time = in.Time
	s.recon = recon
	s.projected = projected
	e.last = Frame{Key: key, Seq: seq, KeyID: keyID, Time: in.Time, Width: cfg.Width, Height: cfg.Height, Precision: cfg.Precision,
		Pose: pose, Expression: expressionValues(qx), Points: recon}
	return packet, nil
}

type Decoder struct {
	s        state
	haveKey  bool
	chainOK  bool
	lastGood [LandmarkCount][2]float64
	goodProj [LandmarkCount][2]float64
}

func NewDecoder() *Decoder { return &Decoder{} }

func PacketKind(packet []byte) int {
	if len(packet) == 0 {
		return -1
	}
	return int(packet[0] >> 6)
}

func (d *Decoder) Reset() { *d = Decoder{} }

func (d *Decoder) Decode(packet []byte) (*Frame, error) {
	if len(packet) < 2 {
		return nil, ErrShort
	}
	if len(packet) > MaxFramePacket {
		return nil, ErrLong
	}
	if int(packet[0]>>4)&3 != Version {
		return nil, ErrVersion
	}
	kind := int(packet[0] >> 6)
	if kind != KindKey && kind != KindDelta {
		if kind == KindReference || kind == KindAtlas {
			return nil, ErrNotAFrame
		}
		return nil, ErrKind
	}
	keyID := int(packet[0] & 15)
	seq := int(packet[1])
	s := &d.s
	key := kind == KindKey
	var cfg Config
	var t uint32
	body := packet[2:]
	if key {
		if len(packet) < keyHeader {
			return nil, ErrShort
		}
		c := Config{
			Width:       int(binary.BigEndian.Uint16(packet[2:4])),
			Height:      int(binary.BigEndian.Uint16(packet[4:6])),
			FPS:         int(packet[6]),
			Precision:   int(packet[7]),
			KeyInterval: 1,
		}
		var err error
		if cfg, err = c.normalizedStrict(); err != nil {
			return nil, ErrHeader
		}
		t = binary.BigEndian.Uint32(packet[8:12])
		body = packet[keyHeader:]
	} else {
		if !d.haveKey || keyID != s.keyID {
			return nil, ErrNeedKey
		}
		ahead := (seq - s.seq) & 255
		if ahead == 0 || ahead > 128 {
			return nil, ErrOld
		}
		cfg = s.cfg
	}
	dec := newRangeDecoder(body)
	var qp qpose
	inSeq := key || (d.chainOK && seq == (s.seq+1)&255)
	if key {
		qp.qx = int(dec.direct(11)) - quatSteps
		qp.qy = int(dec.direct(11)) - quatSteps
		qp.qz = int(dec.direct(11)) - quatSteps
		qp.tx = int(dec.direct(16)) - 32768
		qp.ty = int(dec.direct(16)) - 32768
		qp.s = int(dec.direct(15))
		if qp.qx > quatSteps || qp.qy > quatSteps || qp.qz > quatSteps || qp.s < 1 {
			return nil, ErrCorrupt
		}
	} else {
		nominal := nominalInterval(cfg.FPS)
		timeModel := newIntModel(probHalf)
		dt := dec.int(&timeModel, 0) + nominal
		gap := (seq - s.seq) & 255
		t = s.time + uint32(int32((gap-1)*nominal+dt))
		models := freshPoseModels()
		base := [6]int{s.keyPose.qx, s.keyPose.qy, s.keyPose.qz, s.keyPose.tx, s.keyPose.ty, s.keyPose.s}
		var v [6]int
		for i := range v {
			v[i] = base[i] + dec.int(&models[i], 0)
		}
		qp = qpose{v[0], v[1], v[2], v[3], v[4], v[5]}
		if qp.qx < -quatSteps || qp.qx > quatSteps || qp.qy < -quatSteps || qp.qy > quatSteps || qp.qz < -quatSteps || qp.qz > quatSteps ||
			qp.tx < -32768 || qp.tx > 32767 || qp.ty < -32768 || qp.ty > 32767 || qp.s < 1 || qp.s > 32767 {
			return nil, ErrCorrupt
		}
	}
	var qx [expressionCount]int
	exprModels := freshExpressionModels()
	for i, c := range expressionChannels {
		if c.signed {
			qx[i] = dec.int(&exprModels[i], 0)
		} else {
			qx[i] = dec.uint(&exprModels[i], 0)
		}
		if qx[i] < c.low || qx[i] > c.high {
			return nil, ErrCorrupt
		}
	}
	pose := qp.pose()
	projected := Project(pose)
	frame := &Frame{Key: key, Seq: seq, KeyID: keyID, Time: t, Width: cfg.Width, Height: cfg.Height, Precision: cfg.Precision,
		Pose: pose, Expression: expressionValues(qx)}
	if inSeq {
		var models *landmarkModels
		keyModels := freshLandmarkModels()
		chain := s.chain
		if key {
			chain = freshLandmarkModels()
			models = &keyModels
		} else {
			models = &chain
		}
		step := pose.Scale / float64(cfg.Precision)
		var nonzero [groupCount * 2]int
		var recon [LandmarkCount][2]float64
		for i := 0; i < LandmarkCount; i++ {
			for axis := 0; axis < 2; axis++ {
				var pred float64
				if key {
					pred = projected[i][axis]
				} else {
					pred = s.recon[i][axis] + (projected[i][axis] - s.projected[i][axis])
				}
				class := landmarkGroup(i)*2 + axis
				r := dec.int(&models[class], nonzero[class])
				if r != 0 {
					nonzero[class] = 1
				} else {
					nonzero[class] = 0
				}
				recon[i][axis] = pred + m(float64(r), step)
			}
		}
		if dec.overrun() {
			return nil, ErrCorrupt
		}
		s.chain = chain
		s.recon = recon
		s.projected = projected
		d.lastGood = recon
		d.goodProj = projected
		d.chainOK = true
		frame.Points = recon
	} else {
		if dec.overrun() {
			return nil, ErrCorrupt
		}
		d.chainOK = false
		frame.Stale = true
		for i := 0; i < LandmarkCount; i++ {
			for axis := 0; axis < 2; axis++ {
				frame.Points[i][axis] = d.lastGood[i][axis] + (projected[i][axis] - d.goodProj[i][axis])
			}
		}
	}
	if key {
		d.haveKey = true
		s.cfg = cfg
		s.keyID = keyID
		s.keyPose = qp
	}
	s.seq = seq
	s.time = t
	return frame, nil
}

func (c Config) normalizedStrict() (Config, error) {
	if c.FPS == 0 || c.Precision == 0 {
		return c, ErrConfig
	}
	return c.normalized()
}
