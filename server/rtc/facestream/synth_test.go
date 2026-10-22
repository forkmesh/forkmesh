package facestream

import "math"

type synthOptions struct {
	frames int
	fps    int
	noise  float64
	seed   uint32
	jitter bool
	still  bool
}

type xorshift uint32

func (x *xorshift) next() float64 {
	v := uint32(*x)
	v ^= v << 13
	v ^= v >> 17
	v ^= v << 5
	*x = xorshift(v)
	return float64(v) / 4294967296
}

func (x *xorshift) gauss() float64 {
	a := x.next() + 1e-12
	b := x.next()
	return math.Sqrt(-2*math.Log(a)) * math.Cos(2*math.Pi*b)
}

func eulerQuat(yaw, pitch, roll float64) Quat {
	cy, sy := math.Cos(yaw/2), math.Sin(yaw/2)
	cp, sp := math.Cos(pitch/2), math.Sin(pitch/2)
	cr, sr := math.Cos(roll/2), math.Sin(roll/2)
	qz := Quat{cr, 0, 0, sr}
	qy := Quat{cy, 0, sy, 0}
	qx := Quat{cp, sp, 0, 0}
	return quatMul(quatMul(qz, qy), qx)
}

func quatMul(a, b Quat) Quat {
	return Quat{
		a.W*b.W - a.X*b.X - a.Y*b.Y - a.Z*b.Z,
		a.W*b.X + a.X*b.W + a.Y*b.Z - a.Z*b.Y,
		a.W*b.Y - a.X*b.Z + a.Y*b.W + a.Z*b.X,
		a.W*b.Z + a.X*b.Y - a.Y*b.X + a.Z*b.W,
	}
}

func exact(v float64) float64 { return math.Round(v*1024) / 1024 }

func synthSequence(o synthOptions) ([]Input, [][LandmarkCount][2]float64) {
	rng := xorshift(o.seed | 1)
	var shape [LandmarkCount][3]float64
	for i := range Template {
		shape[i] = Template[i]
		shape[i][0] += 0.03 * (rng.next() - 0.5)
		shape[i][1] += 0.03 * (rng.next() - 0.5)
	}
	inputs := make([]Input, 0, o.frames)
	truths := make([][LandmarkCount][2]float64, 0, o.frames)
	var smooth [LandmarkCount][2]float64
	var clock float64
	for f := 0; f < o.frames; f++ {
		t := float64(f) / float64(o.fps)
		if o.still {
			t = 0
		}
		yaw := 0.3 * math.Sin(2*math.Pi*t/4.3)
		pitch := 0.12 * math.Sin(2*math.Pi*t/3.1+0.7)
		roll := 0.07 * math.Sin(2*math.Pi*t/5.3+1.3)
		scale := 96 * (1 + 0.03*math.Sin(2*math.Pi*t/6.1))
		cx := 640 + 40*math.Sin(2*math.Pi*t/7)
		cy := 330 + 15*math.Sin(2*math.Pi*t/4.9+0.4)
		open := math.Max(0, math.Sin(2*math.Pi*2.7*t)) * (0.5 + 0.5*math.Sin(2*math.Pi*0.4*t))
		smile := 0.5 + 0.5*math.Sin(2*math.Pi*t/8)
		phase := math.Mod(t, 3.2)
		blink := math.Max(0, 1-math.Abs(phase-1.5)/0.08)
		tongue := 0.0
		if math.Mod(t, 5) > 4.2 {
			tongue = 0.8
		}
		gaze := 0.2 * math.Sin(2*math.Pi*t/2.3)
		var posed [LandmarkCount][3]float64
		for i, p := range shape {
			q := p
			switch {
			case i >= 5 && i <= 11:
				q[1] += open * 0.22 * (1 - math.Abs(float64(i-8))/4)
			case i >= 55 && i <= 59, i >= 65 && i <= 67:
				q[1] += open * 0.32
			case i >= 61 && i <= 63:
				q[1] -= open * 0.03
			}
			switch i {
			case 48, 60:
				q[0] -= smile * 0.05
				q[1] -= smile * 0.04
			case 54, 64:
				q[0] += smile * 0.05
				q[1] -= smile * 0.04
			case 37, 38, 43, 44:
				q[1] += blink * 0.09
			case 40, 41, 46, 47:
				q[1] -= blink * 0.02
			}
			posed[i] = q
		}
		pose := Pose{Rotation: eulerQuat(yaw, pitch, roll), X: cx, Y: cy, Scale: scale}
		r := RotationRows(pose.Rotation)
		var truth [LandmarkCount][2]float64
		var seen [LandmarkCount][2]float64
		for i := range posed {
			truth[i][0] = cx + scale*dot3(r[0], posed[i])
			truth[i][1] = cy + scale*dot3(r[1], posed[i])
			for axis := 0; axis < 2; axis++ {
				noisy := truth[i][axis] + o.noise*rng.gauss()
				if f == 0 {
					smooth[i][axis] = noisy
				} else {
					smooth[i][axis] += 0.55 * (noisy - smooth[i][axis])
				}
				seen[i][axis] = exact(smooth[i][axis])
			}
		}
		step := 1000 / float64(o.fps)
		if o.jitter {
			step += 6 * (rng.next() - 0.5)
		}
		if f > 0 {
			clock += step
		}
		in := Input{
			Time:   uint32(1000 + math.Floor(clock)),
			Points: seen,
			Pose: Pose{
				Rotation: Quat{exact(pose.Rotation.W), exact(pose.Rotation.X), exact(pose.Rotation.Y), exact(pose.Rotation.Z)},
				X:        exact(cx + o.noise*rng.gauss()*0.3),
				Y:        exact(cy + o.noise*rng.gauss()*0.3),
				Scale:    exact(scale),
			},
			Expression: Expression{
				LidLeft: exact(blink), LidRight: exact(blink),
				GazeX: exact(gaze), GazeY: exact(gaze * 0.3),
				Mouth: exact(open), Smile: exact(smile), Tongue: exact(tongue),
			},
		}
		inputs = append(inputs, in)
		truths = append(truths, truth)
	}
	return inputs, truths
}
