package fmmmobile

import (
	"errors"

	"forkmesh.com/mirror-node/rtc/e2ee"
	"forkmesh.com/mirror-node/rtc/fmm"
)

const (
	EventJoined             = fmm.EventJoined
	EventTrackAdded         = fmm.EventTrackAdded
	EventTrackRemoved       = fmm.EventTrackRemoved
	EventKeyframeRequest    = fmm.EventKeyframeRequest
	EventBitrateHint        = fmm.EventBitrateHint
	EventError              = fmm.EventError
	EventRecover            = fmm.EventRecover
	EventSendRepresentation = fmm.EventSendRepresentation
	EventReferencePicture   = fmm.EventReferencePicture
	EventRepresentation     = fmm.EventRepresentation
	EventFaceReference      = fmm.EventFaceReference
	EventChat               = 20
	EventAuth               = 21
	EventVerify             = 22
	EventMoved              = 23
	EventClosed             = 24
	EventTarget             = 25
	EventPath               = 26

	KindAudio = fmm.KindAudio
	KindVideo = fmm.KindVideo

	RepOff   = fmm.RepOff
	RepVideo = fmm.RepVideo
	RepFace  = fmm.RepFace

	AudioWaiting = fmm.AudioWaiting
	AudioPlay    = fmm.AudioPlay
	AudioConceal = fmm.AudioConceal
	AudioInsert  = fmm.AudioInsert
	AudioDRED    = fmm.AudioDRED

	FlagKeyframe = 1
	FlagLongTerm = 2
	FlagRecover  = 4
	FlagFace     = 8

	AuthPending  = int(e2ee.AuthPending)
	AuthVerified = int(e2ee.AuthVerified)
	AuthMissing  = int(e2ee.AuthMissing)
	AuthForged   = int(e2ee.AuthForged)

	ChatPresence = "presence"
	ChatKey      = "key"
	ChatVerify   = "verify"

	SealOverhead = e2ee.FrameTag + e2ee.FrameTrailer
	FrameUs      = 20_000
)

var (
	ErrConfig    = errors.New("fmmmobile: incomplete call configuration")
	ErrNotLive   = errors.New("fmmmobile: call not connected")
	ErrNoKey     = errors.New("fmmmobile: no media key yet")
	ErrSender    = errors.New("fmmmobile: chat sender does not match the message identity")
	ErrUnknown   = errors.New("fmmmobile: unknown chat message kind")
	ErrNoPath    = errors.New("fmmmobile: no second path")
	ErrPlatform  = errors.New("fmmmobile: binding a socket to an interface is not supported here")
	ErrDuplicate = errors.New("fmmmobile: already connected")
)

type Listener interface {
	OnEvent(e *Event)
}

type Config struct {
	Access       string
	Call         string
	Peer         string
	Account      string
	Identity     []byte
	StartBitrate int
	MinBitrate   int
	MaxBitrate   int
	LocalAddr    string
}

type Event struct {
	Kind     int
	Stream   int
	Track    int
	Layers   int
	Peer     string
	Name     string
	Bitrate  int
	Code     int
	Text     string
	FrameID  int64
	Rep      int
	MaxBytes int
	State    int
	Data     []byte
}

type AudioOut struct {
	Status int
	Offset int
	Data   []byte
}

type VideoIn struct {
	Stream    int
	FrameID   int64
	Keyframe  bool
	Layer     int
	Timestamp int64
	Face      bool
	Recovery  bool
	LongTerm  bool
	Data      []byte
}

type Verification struct {
	Round      string
	Gen        int
	HasCode    bool
	Emoji      string
	EmojiNames string
	Indices    string
	Digits     string
	QR         string
	Waiting    string
	Late       bool
	Rounds     int
	Unfinished int
	Verified   bool
	Newcomers  string
	NeedCommit bool
}

type Stats struct {
	TargetBitrate    int
	VideoBitrate     int
	RTTUs            int64
	Loss             float64
	SentPackets      int
	ReceivedPackets  int
	RecoveredPackets int
	AudioPlayed      int
	AudioConcealed   int
	AudioFEC         int
	AudioDRED        int
	AudioInserted    int
	VideoReleased    int
	VideoUndecodable int
	KeyframeRequests int
	RecoveryRequests int
	RecoveryFrames   int
	Recoveries       int
	Sealed           int
	Opened           int
	Refused          int
	AuthSent         int
	AuthAccepted     int
	Paths            int
	UsablePaths      int
	Moved            bool
	DeadAfterMs      int64
	MovedAfterMs     int64
	Goroutines       int
}

type AuthStatus struct {
	State    int
	Dropping bool
	Verified int64
	Missing  int64
	Forged   int64
}
