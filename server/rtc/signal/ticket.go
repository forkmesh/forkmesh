package signal

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"regexp"
	"strings"
	"time"
)

const TicketLifetime = 60 * time.Second

var (
	ErrTicketMalformed = errors.New("ticket malformed")
	ErrTicketSignature = errors.New("ticket signature invalid")
	ErrTicketExpired   = errors.New("ticket expired")

	roomPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)
	peerPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)
)

type Ticket struct {
	Room string `json:"room"`
	Peer string `json:"peer"`
	Exp  int64  `json:"exp"`
}

func RoomID(secret []byte, channelID string) string {
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte("forkmesh-call-room-v1\n" + channelID))
	return hex.EncodeToString(mac.Sum(nil))
}

func NewPeerID() string {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		panic(err)
	}
	return hex.EncodeToString(raw[:])
}

func MintTicket(secret []byte, ticket Ticket) string {
	body, _ := json.Marshal(ticket)
	payload := base64.RawURLEncoding.EncodeToString(body)
	return payload + "." + base64.RawURLEncoding.EncodeToString(ticketMAC(secret, payload))
}

func VerifyTicket(secret []byte, token string, now time.Time) (Ticket, error) {
	if len(token) > 1024 {
		return Ticket{}, ErrTicketMalformed
	}
	payload, signature, found := strings.Cut(token, ".")
	if !found || payload == "" || signature == "" {
		return Ticket{}, ErrTicketMalformed
	}
	given, err := base64.RawURLEncoding.DecodeString(signature)
	if err != nil {
		return Ticket{}, ErrTicketMalformed
	}
	if !hmac.Equal(given, ticketMAC(secret, payload)) {
		return Ticket{}, ErrTicketSignature
	}
	body, err := base64.RawURLEncoding.DecodeString(payload)
	if err != nil {
		return Ticket{}, ErrTicketMalformed
	}
	var ticket Ticket
	if err := json.Unmarshal(body, &ticket); err != nil {
		return Ticket{}, ErrTicketMalformed
	}
	if !roomPattern.MatchString(ticket.Room) || !peerPattern.MatchString(ticket.Peer) {
		return Ticket{}, ErrTicketMalformed
	}
	if now.Unix() > ticket.Exp || ticket.Exp > now.Add(2*TicketLifetime).Unix() {
		return Ticket{}, ErrTicketExpired
	}
	return ticket, nil
}

func ticketMAC(secret []byte, payload string) []byte {
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte("forkmesh-call-ticket-v1\n" + payload))
	return mac.Sum(nil)
}
