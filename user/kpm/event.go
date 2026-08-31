package kpm

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"hash/crc32"
	"strings"
)

const (
	EventMagic    uint32 = 0x455a5053
	EventWireSize        = 432
)

type Registers struct {
	X      [31]uint64
	SP     uint64
	PC     uint64
	PState uint64
}

type Event struct {
	Type             uint16
	Flags            uint16
	CPU              uint32
	Sequence         uint64
	Timestamp        uint64
	BindingID        uint32
	BreakpointID     uint32
	Generation       uint64
	TaskCookie       uint64
	PID              uint32
	TGID             uint32
	UID              uint32
	StartTime        uint64
	StartBootTime    uint64
	Comm             string
	ExceptionClass   uint32
	SlotKind         uint16
	SlotIndex        uint16
	RequestedAddress uint64
	ObservedAddress  uint64
	Value            uint64
	Control          uint32
	ObservedControl  uint32
	MDSCR            uint64
	Registers        Registers
}

func DecodeEventHex(encoded string) (Event, error) {
	var event Event
	if len(encoded) != EventWireSize*2 {
		return event, fmt.Errorf("%w: encoded length", ErrMalformedEvent)
	}
	raw, err := hex.DecodeString(encoded)
	if err != nil || len(raw) != EventWireSize {
		return event, fmt.Errorf("%w: hex", ErrMalformedEvent)
	}
	order := binary.LittleEndian
	if order.Uint32(raw[0:4]) != EventMagic {
		return event, fmt.Errorf("%w: magic", ErrMalformedEvent)
	}
	version := order.Uint16(raw[4:6])
	if version != ProtocolVersion {
		return event, fmt.Errorf("%w: event version %d", ErrUnsupportedVersion, version)
	}
	if order.Uint16(raw[6:8]) != EventWireSize {
		return event, fmt.Errorf("%w: declared size", ErrMalformedEvent)
	}
	wantCRC := order.Uint32(raw[428:432])
	if gotCRC := crc32.ChecksumIEEE(raw[:428]); gotCRC != wantCRC {
		return event, fmt.Errorf("%w: got %#x want %#x", ErrEventChecksum, gotCRC, wantCRC)
	}
	event.Type = order.Uint16(raw[8:10])
	event.Flags = order.Uint16(raw[10:12])
	event.CPU = order.Uint32(raw[12:16])
	event.Sequence = order.Uint64(raw[16:24])
	event.Timestamp = order.Uint64(raw[24:32])
	event.BindingID = order.Uint32(raw[32:36])
	event.BreakpointID = order.Uint32(raw[36:40])
	event.Generation = order.Uint64(raw[40:48])
	event.TaskCookie = order.Uint64(raw[48:56])
	event.PID = order.Uint32(raw[56:60])
	event.TGID = order.Uint32(raw[60:64])
	event.UID = order.Uint32(raw[64:68])
	event.StartTime = order.Uint64(raw[72:80])
	event.StartBootTime = order.Uint64(raw[80:88])
	event.Comm = strings.TrimRight(string(raw[88:104]), "\x00")
	event.ExceptionClass = order.Uint32(raw[104:108])
	event.SlotKind = order.Uint16(raw[108:110])
	event.SlotIndex = order.Uint16(raw[110:112])
	event.RequestedAddress = order.Uint64(raw[112:120])
	event.ObservedAddress = order.Uint64(raw[120:128])
	event.Value = order.Uint64(raw[128:136])
	event.Control = order.Uint32(raw[136:140])
	event.ObservedControl = order.Uint32(raw[140:144])
	event.MDSCR = order.Uint64(raw[144:152])
	for index := range event.Registers.X {
		event.Registers.X[index] = order.Uint64(raw[152+index*8 : 160+index*8])
	}
	event.Registers.SP = order.Uint64(raw[400:408])
	event.Registers.PC = order.Uint64(raw[408:416])
	event.Registers.PState = order.Uint64(raw[416:424])
	return event, nil
}
