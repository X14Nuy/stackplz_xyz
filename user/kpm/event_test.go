package kpm

import (
	"encoding/binary"
	"encoding/hex"
	"errors"
	"hash/crc32"
	"testing"
)

func eventFixture() []byte {
	raw := make([]byte, 432)
	put16 := func(offset int, value uint16) { binary.LittleEndian.PutUint16(raw[offset:], value) }
	put32 := func(offset int, value uint32) { binary.LittleEndian.PutUint32(raw[offset:], value) }
	put64 := func(offset int, value uint64) { binary.LittleEndian.PutUint64(raw[offset:], value) }
	put32(0, 0x455a5053)
	put16(4, 1)
	put16(6, 432)
	put16(8, 2)
	put16(10, 0x11)
	put32(12, 5)
	put64(16, 0x0102030405060708)
	put64(24, 9988776655)
	put32(32, 9)
	put32(36, 7)
	put64(40, 33)
	put64(48, 0x123456789abcdef0)
	put32(56, 31337)
	put32(60, 31300)
	put32(64, 10234)
	put64(72, 111222333)
	put64(80, 444555666)
	copy(raw[88:104], []byte("target.proc"))
	put32(104, 0x22)
	put16(108, 1)
	put16(110, 4)
	put64(112, 0x7abc1000)
	put64(120, 0x7abc1000)
	put64(128, 0x7abc1000)
	put32(136, 0x1e5)
	put32(140, 0x1e5)
	put64(144, 0x8000)
	for index := 0; index < 31; index++ {
		put64(152+index*8, uint64(index)+0x100)
	}
	put64(400, 0x7ffffff000)
	put64(408, 0x7abc1000)
	put64(416, 0x600003c0)
	put32(428, crc32.ChecksumIEEE(raw[:428]))
	return raw
}

func TestDecodeEventHexReadsEveryField(t *testing.T) {
	raw := eventFixture()
	event, err := DecodeEventHex(hex.EncodeToString(raw))
	if err != nil {
		t.Fatal(err)
	}
	if event.Sequence != 0x0102030405060708 || event.Timestamp != 9988776655 || event.CPU != 5 {
		t.Fatalf("header=%#v", event)
	}
	if event.BindingID != 9 || event.BreakpointID != 7 || event.Generation != 33 {
		t.Fatalf("binding=%#v", event)
	}
	if event.TaskCookie != 0x123456789abcdef0 || event.PID != 31337 || event.TGID != 31300 || event.UID != 10234 || event.Comm != "target.proc" {
		t.Fatalf("identity=%#v", event)
	}
	if event.RequestedAddress != 0x7abc1000 || event.ObservedAddress != 0x7abc1000 || event.SlotIndex != 4 || event.Control != 0x1e5 {
		t.Fatalf("debug=%#v", event)
	}
	for index, value := range event.Registers.X {
		if value != uint64(index)+0x100 {
			t.Fatalf("x%d=%#x", index, value)
		}
	}
	if event.Registers.SP != 0x7ffffff000 || event.Registers.PC != 0x7abc1000 || event.Registers.PState != 0x600003c0 {
		t.Fatalf("register tail=%#v", event.Registers)
	}
}

func TestDecodeEventHexRejectsMalformedRecords(t *testing.T) {
	valid := eventFixture()
	tests := []struct {
		name string
		raw  []byte
		err  error
	}{
		{name: "truncated", raw: append([]byte(nil), valid[:431]...), err: ErrMalformedEvent},
		{name: "extra", raw: append(append([]byte(nil), valid...), 0), err: ErrMalformedEvent},
		{name: "bad magic", raw: mutateEvent(valid, 0, 0xff), err: ErrMalformedEvent},
		{name: "bad version", raw: mutateEvent(valid, 4, 2), err: ErrUnsupportedVersion},
		{name: "bad declared size", raw: mutateEvent(valid, 6, 0), err: ErrMalformedEvent},
		{name: "bad crc", raw: mutateEvent(valid, 200, 0xff), err: ErrEventChecksum},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			_, err := DecodeEventHex(hex.EncodeToString(test.raw))
			if !errors.Is(err, test.err) {
				t.Fatalf("got %v, want %v", err, test.err)
			}
		})
	}
	if _, err := DecodeEventHex("not-hex"); !errors.Is(err, ErrMalformedEvent) {
		t.Fatalf("non-hex error=%v", err)
	}
}

func mutateEvent(input []byte, offset int, value byte) []byte {
	output := append([]byte(nil), input...)
	output[offset] = value
	return output
}
