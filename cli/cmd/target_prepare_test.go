package cmd

import (
	"context"
	"errors"
	"strings"
	"testing"

	"stackplz/user/kpm"
)

type fakeTargetResolver struct {
	procUID     uint32
	procErr     error
	kpmIdentity kpm.Identity
	kpmErr      error
	procCalls   int
	kpmCalls    int
}

func (fake *fakeTargetResolver) UIDFromProc(uint32) (uint32, error) {
	fake.procCalls++
	return fake.procUID, fake.procErr
}

func (fake *fakeTargetResolver) IdentityFromKPM(context.Context, uint32) (kpm.Identity, error) {
	fake.kpmCalls++
	return fake.kpmIdentity, fake.kpmErr
}

func TestPreparePIDTargetsKPMNeverCallsProc(t *testing.T) {
	fake := &fakeTargetResolver{kpmIdentity: kpm.Identity{PID: 31337, TGID: 31337, UID: 10234}}
	got, err := preparePIDTargets(context.Background(), fake, TargetSourceKPM, []uint32{31337}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if fake.procCalls != 0 {
		t.Fatalf("proc called %d times", fake.procCalls)
	}
	if fake.kpmCalls != 1 || len(got) != 1 || got[0].UID != 10234 || got[0].Source != TargetSourceKPM {
		t.Fatalf("unexpected resolution: calls=%d got=%#v", fake.kpmCalls, got)
	}
	if got[0].Identity == nil || got[0].Identity.PID != 31337 {
		t.Fatalf("identity not retained: %#v", got[0])
	}
}

func TestPreparePIDTargetsProcNeverCallsKPM(t *testing.T) {
	fake := &fakeTargetResolver{procUID: 10001}
	got, err := preparePIDTargets(context.Background(), fake, TargetSourceProc, []uint32{41}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if fake.procCalls != 1 || fake.kpmCalls != 0 || got[0].UID != 10001 || got[0].Source != TargetSourceProc {
		t.Fatalf("unexpected resolution: proc=%d kpm=%d got=%#v", fake.procCalls, fake.kpmCalls, got)
	}
}

func TestPreparePIDTargetsAutoFallsBackOnce(t *testing.T) {
	procErr := errors.New("proc hidden")
	fake := &fakeTargetResolver{
		procErr:     procErr,
		kpmIdentity: kpm.Identity{PID: 99, TGID: 99, UID: 10123},
	}
	got, err := preparePIDTargets(context.Background(), fake, TargetSourceAuto, []uint32{99}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if fake.procCalls != 1 || fake.kpmCalls != 1 || got[0].Source != TargetSourceKPM || got[0].UID != 10123 {
		t.Fatalf("unexpected fallback: proc=%d kpm=%d got=%#v", fake.procCalls, fake.kpmCalls, got)
	}
}

func TestPreparePIDTargetsAutoWithUIDStillFallsBackWhenProcIsHidden(t *testing.T) {
	uid := uint32(12345)
	fake := &fakeTargetResolver{
		procErr:     errors.New("proc hidden"),
		kpmIdentity: kpm.Identity{PID: 99, TGID: 99, UID: 55555},
	}
	got, err := preparePIDTargets(context.Background(), fake, TargetSourceAuto, []uint32{99}, &uid)
	if err != nil {
		t.Fatal(err)
	}
	if fake.procCalls != 1 || fake.kpmCalls != 1 || got[0].Source != TargetSourceKPM || got[0].UID != uid {
		t.Fatalf("unexpected fallback: proc=%d kpm=%d got=%#v", fake.procCalls, fake.kpmCalls, got)
	}
}

func TestPreparePIDTargetsAutoPreservesBothErrors(t *testing.T) {
	procErr := errors.New("proc hidden")
	kpmErr := errors.New("observer unavailable")
	fake := &fakeTargetResolver{procErr: procErr, kpmErr: kpmErr}
	_, err := preparePIDTargets(context.Background(), fake, TargetSourceAuto, []uint32{99}, nil)
	if err == nil {
		t.Fatal("expected combined resolution error")
	}
	if !errors.Is(err, procErr) || !errors.Is(err, kpmErr) {
		t.Fatalf("causes not preserved: %v", err)
	}
	if fake.procCalls != 1 || fake.kpmCalls != 1 {
		t.Fatalf("wrong call counts: proc=%d kpm=%d", fake.procCalls, fake.kpmCalls)
	}
}

func TestPreparePIDTargetsExplicitUIDBypassesLookups(t *testing.T) {
	uid := uint32(12345)
	fake := &fakeTargetResolver{procErr: errors.New("must not run"), kpmErr: errors.New("must not run")}
	got, err := preparePIDTargets(context.Background(), fake, TargetSourceProc, []uint32{7}, &uid)
	if err != nil {
		t.Fatal(err)
	}
	if fake.procCalls != 0 || fake.kpmCalls != 0 || got[0].UID != uid {
		t.Fatalf("override performed lookup: %#v calls=%d/%d", got, fake.procCalls, fake.kpmCalls)
	}
}

func TestPreparePIDTargetsKPMUIDOverrideStillWaitsForIdentity(t *testing.T) {
	uid := uint32(12345)
	fake := &fakeTargetResolver{kpmIdentity: kpm.Identity{PID: 7, TGID: 7, UID: 55555}}
	got, err := preparePIDTargets(context.Background(), fake, TargetSourceKPM, []uint32{7}, &uid)
	if err != nil {
		t.Fatal(err)
	}
	if fake.procCalls != 0 || fake.kpmCalls != 1 || got[0].UID != uid || got[0].Identity == nil {
		t.Fatalf("KPM override skipped identity: %#v calls=%d/%d", got, fake.procCalls, fake.kpmCalls)
	}
}

func TestPreparePIDTargetsRejectsMismatchedKPMIdentity(t *testing.T) {
	fake := &fakeTargetResolver{kpmIdentity: kpm.Identity{PID: 8, TGID: 8, UID: 10000}}
	_, err := preparePIDTargets(context.Background(), fake, TargetSourceKPM, []uint32{7}, nil)
	if err == nil || !strings.Contains(err.Error(), "requested pid 7") {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestParseTargetSource(t *testing.T) {
	for _, value := range []TargetSource{TargetSourceProc, TargetSourceKPM, TargetSourceAuto} {
		got, err := ParseTargetSource(string(value))
		if err != nil || got != value {
			t.Fatalf("ParseTargetSource(%q)=(%q,%v)", value, got, err)
		}
	}
	if _, err := ParseTargetSource("hidden"); err == nil {
		t.Fatal("unknown source accepted")
	}
}

func TestSingleBreakpointPIDRejectsMultiple(t *testing.T) {
	if _, err := singleBreakpointPID([]uint32{1, 2}); err == nil || !strings.Contains(err.Error(), "exactly one") {
		t.Fatalf("unexpected error: %v", err)
	}
	if got, err := singleBreakpointPID([]uint32{9}); err != nil || got != 9 {
		t.Fatalf("single pid=(%d,%v)", got, err)
	}
}
