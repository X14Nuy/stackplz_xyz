package cmd

import (
	"os"
	"path/filepath"
	"testing"

	"stackplz/user/event"
)

func TestResolveBreakpointBasePrecedence(t *testing.T) {
	got, err := resolveBreakpointBase(70001, "libtarget.so", filepath.Join(t.TempDir(), "missing"), 0x70000000)
	if err != nil || got != 0x70000000 {
		t.Fatalf("explicit base=(0x%x,%v)", got, err)
	}

	mapsFile := filepath.Join(t.TempDir(), "maps.txt")
	content := "70001000-70002000 r--p 00000000 00:00 0 /data/app/example/lib/arm64/libtarget.so\n" +
		"70002000-70003000 r-xp 00001000 00:00 0 /data/app/example/lib/arm64/libtarget.so\n"
	if err := os.WriteFile(mapsFile, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	got, err = resolveBreakpointBase(70002, "libtarget.so", mapsFile, 0)
	if err != nil || got != 0x70001000 {
		t.Fatalf("offline base=(0x%x,%v)", got, err)
	}
}

func TestResolveBreakpointBaseKPMUsesInjectedMaps(t *testing.T) {
	const pid = 70003
	content := []byte("70003000-70004000 r-xp 0 00:00 0 /hidden/libtarget.so\n")
	if err := event.LoadMapsContent(pid, content); err != nil {
		t.Fatal(err)
	}
	got, err := resolveBreakpointBaseForSource(TargetSourceKPM, pid, "libtarget.so", "", 0)
	if err != nil || got != 0x70003000 {
		t.Fatalf("KPM maps base=(0x%x,%v)", got, err)
	}
}

func TestResolveBreakpointBaseRejectsInvalidSourceBeforeResolution(t *testing.T) {
	_, err := resolveBreakpointBaseForSource(TargetSource("invalid"), 70004, "libtarget.so", "", 0x70000000)
	if err == nil {
		t.Fatal("invalid source accepted")
	}
}
