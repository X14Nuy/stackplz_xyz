package kpm

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"
)

func TestBindCommandCanonicalOrder(t *testing.T) {
	uid := uint32(10234)
	start := uint64(99887766)
	got, err := BindCommand(BindingRequest{
		PID: 31337, Mode: BindPID, UID: &uid, Comm: "target.proc",
		StartBootTime: &start,
	})
	if err != nil {
		t.Fatal(err)
	}
	want := "bind pid=31337 mode=pid uid=10234 comm=target.proc start=99887766"
	if got != want {
		t.Fatalf("command mismatch\n got: %q\nwant: %q", got, want)
	}
}

func TestBindCommandRejectsUnsafeValues(t *testing.T) {
	tests := []struct {
		name string
		req  BindingRequest
	}{
		{name: "zero pid", req: BindingRequest{Mode: BindPID}},
		{name: "unknown mode", req: BindingRequest{PID: 1, Mode: BindMode("thread-list")}},
		{name: "space in comm", req: BindingRequest{PID: 1, Mode: BindPID, Comm: "bad name"}},
		{name: "control in comm", req: BindingRequest{PID: 1, Mode: BindPID, Comm: "bad\nname"}},
		{name: "oversized comm", req: BindingRequest{PID: 1, Mode: BindPID, Comm: strings.Repeat("a", 16)}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if _, err := BindCommand(test.req); err == nil {
				t.Fatal("unsafe binding was accepted")
			}
		})
	}
}

func TestBreakCommandCanonicalOrder(t *testing.T) {
	got, err := BreakCommand(Breakpoint{
		ID: 7, Kind: BreakExecute, Address: 0x7abc1000,
		Length: 4, Mode: BreakRepeat,
	})
	if err != nil {
		t.Fatal(err)
	}
	want := "break id=7 kind=x addr=0x7abc1000 len=4 mode=repeat"
	if got != want {
		t.Fatalf("command mismatch\n got: %q\nwant: %q", got, want)
	}
}

func TestBreakCommandRejectsInvalidAddressAndShape(t *testing.T) {
	tests := []struct {
		name string
		bp   Breakpoint
		err  error
	}{
		{name: "kernel address", bp: Breakpoint{ID: 1, Kind: BreakExecute, Address: 0xffff000000001000, Length: 4, Mode: BreakOnce}, err: ErrInvalidAddress},
		{name: "unaligned execute", bp: Breakpoint{ID: 1, Kind: BreakExecute, Address: 0x1002, Length: 4, Mode: BreakOnce}, err: ErrInvalidAddress},
		{name: "execute wrong length", bp: Breakpoint{ID: 1, Kind: BreakExecute, Address: 0x1000, Length: 2, Mode: BreakOnce}, err: ErrInvalidLength},
		{name: "watch crosses window", bp: Breakpoint{ID: 1, Kind: BreakWrite, Address: 0x1007, Length: 2, Mode: BreakOnce}, err: ErrInvalidAddress},
		{name: "watch bad length", bp: Breakpoint{ID: 1, Kind: BreakRead, Address: 0x1000, Length: 3, Mode: BreakOnce}, err: ErrInvalidLength},
		{name: "zero id", bp: Breakpoint{Kind: BreakExecute, Address: 0x1000, Length: 4, Mode: BreakOnce}, err: ErrInvalidID},
		{name: "unknown kind", bp: Breakpoint{ID: 1, Kind: BreakKind("io"), Address: 0x1000, Length: 4, Mode: BreakOnce}, err: ErrInvalidKind},
		{name: "unknown mode", bp: Breakpoint{ID: 1, Kind: BreakExecute, Address: 0x1000, Length: 4, Mode: BreakMode("forever")}, err: ErrInvalidMode},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			_, err := BreakCommand(test.bp)
			if !errors.Is(err, test.err) {
				t.Fatalf("got %v, want %v", err, test.err)
			}
		})
	}
}

func TestMapsReadCommandCanonicalOrder(t *testing.T) {
	got, err := MapsReadCommand(7, 1536)
	if err != nil {
		t.Fatal(err)
	}
	if got != "maps-read snapshot=7 offset=1536" {
		t.Fatalf("command=%q", got)
	}
	if _, err := MapsReadCommand(0, 0); !errors.Is(err, ErrInvalidID) {
		t.Fatalf("zero snapshot error=%v", err)
	}
}

func TestExecRunnerPassesOneControlArgumentWithoutShell(t *testing.T) {
	var gotBinary string
	var gotArgs []string
	runner := &ExecRunner{
		binary: "/data/adb/modules/KPatch-Next/bin/kpatch",
		module: "stackplz-kpm",
		run: func(_ context.Context, binary string, args ...string) ([]byte, error) {
			gotBinary = binary
			gotArgs = append([]string(nil), args...)
			return []byte("status=0 version=1\nstate=ready\n"), nil
		},
	}
	if _, err := runner.Control(context.Background(), "bind pid=7 mode=pid"); err != nil {
		t.Fatal(err)
	}
	if gotBinary != "/data/adb/modules/KPatch-Next/bin/kpatch" {
		t.Fatalf("binary=%q", gotBinary)
	}
	want := []string{"kpm", "ctl0", "stackplz-kpm", "bind pid=7 mode=pid"}
	if !reflect.DeepEqual(gotArgs, want) {
		t.Fatalf("args=%q want=%q", gotArgs, want)
	}
}

func TestParseResponseRejectsDuplicateAndUnknownVersion(t *testing.T) {
	if _, err := parseResponse("status=0 version=1\nstate=ready\nstate=pending\n"); !errors.Is(err, ErrMalformedResponse) {
		t.Fatalf("duplicate key error=%v", err)
	}
	if _, err := parseResponse("status=0 version=2\nstate=ready\n"); !errors.Is(err, ErrUnsupportedVersion) {
		t.Fatalf("version error=%v", err)
	}
}

func TestParseResponseReturnsTypedControlError(t *testing.T) {
	_, err := parseResponse("status=-16 version=1\nreason=busy\n")
	var controlErr *ControlError
	if !errors.As(err, &controlErr) || controlErr.Status != -16 || controlErr.Reason != "busy" {
		t.Fatalf("error=%#v", err)
	}
}
