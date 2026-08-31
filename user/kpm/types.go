package kpm

import (
	"errors"
	"strconv"
)

const (
	ProtocolVersion      = 1
	MaxCommandBytes      = 512
	MaxResponseBytes     = 4096
	CommBytes            = 16
	MaxMapsSnapshotBytes = 16 * 1024 * 1024
	MaxMapsChunkBytes    = 1536
)

var (
	ErrInvalidAddress     = errors.New("invalid breakpoint address")
	ErrInvalidLength      = errors.New("invalid breakpoint length")
	ErrInvalidID          = errors.New("invalid identifier")
	ErrInvalidKind        = errors.New("invalid breakpoint kind")
	ErrInvalidMode        = errors.New("invalid mode")
	ErrInvalidCommand     = errors.New("invalid control command")
	ErrMalformedResponse  = errors.New("malformed control response")
	ErrUnsupportedVersion = errors.New("unsupported control protocol version")
	ErrMalformedEvent     = errors.New("malformed KPM event")
	ErrEventChecksum      = errors.New("KPM event checksum mismatch")
	ErrProfileMismatch    = errors.New("KPM device profile mismatch")
	ErrNotReady           = errors.New("KPM is not ready")
	ErrBindingStale       = errors.New("KPM binding is stale")
	ErrBindingExited      = errors.New("KPM target exited")
	ErrMapsUnsupported    = errors.New("KPM maps are unsupported")
	ErrMapsChecksum       = errors.New("KPM maps checksum mismatch")
)

type BindMode string

const (
	BindPID    BindMode = "pid"
	BindTGID   BindMode = "tgid"
	BindEither BindMode = "either"
)

type BreakKind string

const (
	BreakExecute   BreakKind = "x"
	BreakRead      BreakKind = "r"
	BreakWrite     BreakKind = "w"
	BreakReadWrite BreakKind = "rw"
)

type BreakMode string

const (
	BreakOnce   BreakMode = "once"
	BreakRepeat BreakMode = "repeat"
)

type BindingRequest struct {
	PID           uint32
	Mode          BindMode
	UID           *uint32
	Comm          string
	StartBootTime *uint64
}

type Breakpoint struct {
	ID      uint32
	Kind    BreakKind
	Address uint64
	Length  uint8
	Mode    BreakMode
}

type Identity struct {
	Generation    uint64
	TaskCookie    uint64
	PID           uint32
	TGID          uint32
	UID           uint32
	StartTime     uint64
	StartBootTime uint64
	Comm          string
}

type ModuleStatus struct {
	State                 string
	Profile               string
	Binding               string
	Identity              *Identity
	RequestID             uint64
	RequestState          string
	RequestStatus         int
	RequestPresent        bool
	MapsCapabilityPresent bool
	MapsSupported         bool
	MapsState             string
	MapsSnapshot          uint64
	MapsSize              uint32
}

type Response struct {
	Status  int
	Version uint16
	Fields  map[string]string
}

type ControlError struct {
	Status int
	Reason string
}

func (e *ControlError) Error() string {
	if e.Reason == "" {
		return "KPM control failed"
	}
	return "KPM control failed: " + e.Reason
}

type AsyncRequestError struct {
	RequestID uint64
	Status    int
}

func (e *AsyncRequestError) Error() string {
	return "KPM async request " + strconv.FormatUint(e.RequestID, 10) +
		" failed with status " + strconv.Itoa(e.Status)
}
