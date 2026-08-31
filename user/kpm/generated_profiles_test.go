package kpm

import "testing"

func TestFindDeviceProfileReturnsExactPLK110Facts(t *testing.T) {
	profile, found := FindDeviceProfile("oneplus-plk110-a16-b4999618-d05")
	if !found {
		t.Fatal("generated PLK110 profile was not found")
	}
	if profile.Task.PID != 1800 || profile.Task.Comm != 2320 || profile.Cred.UID != 8 {
		t.Fatalf("wrong generated task layout: %#v %#v", profile.Task, profile.Cred)
	}
	if profile.Debug.BRPs != 6 || profile.Debug.WRPs != 4 || profile.Debug.MaxCPUs != 8 {
		t.Fatalf("wrong generated debug capabilities: %#v", profile.Debug)
	}
	if profile.Kernel.ACKCommit != "b2a876903b495c444a94b16f50d1463ffe953957" {
		t.Fatalf("wrong ACK commit: %s", profile.Kernel.ACKCommit)
	}
	if profile.Kernel.TaskCommLen != 16 || profile.Kernel.LinuxBannerCapacity != 256 {
		t.Fatalf("wrong generated kernel ABI: %#v", profile.Kernel)
	}
	if profile.KPatch.ControlPath != "/data/adb/modules/KPatch-Next/bin/kpatch" ||
		profile.KPatch.ModuleName != "stackplz-kpm" ||
		profile.KPatch.SafeUnloadSymbol != "kpm_safe_unload_v1" ||
		!profile.KPatch.KFuncExportsArePointers {
		t.Fatalf("wrong generated KPatch ABI: %#v", profile.KPatch)
	}
	if profile.Hooks.FinishTaskSwitchArgs != 1 || profile.Hooks.BreakpointHandlerArgs != 3 {
		t.Fatalf("wrong generated hook ABI: %#v", profile.Hooks)
	}
	if profile.Quirks.SafeUnloadRequired || !profile.Quirks.LinuxBannerPrefixOK {
		t.Fatalf("wrong generated quirks: %#v", profile.Quirks)
	}
	if profile.Maps.MaxSnapshotBytes != 2*1024*1024 ||
		profile.Maps.MaxChunkBytes != 1536 ||
		profile.Maps.SeqFileSize != 136 ||
		profile.Maps.SeqPrivate != 128 ||
		profile.Maps.ProcIter != 24 ||
		profile.Maps.VMAIteratorSize != 72 ||
		profile.Maps.MMMapleTree != 64 ||
		profile.Maps.ShowMapVMAArgs != 2 {
		t.Fatalf("wrong generated maps ABI: %#v", profile.Maps)
	}
	if profile.Symbols.ShowMapVMA != "show_map_vma" ||
		profile.Symbols.VmallocNoprof != "vmalloc_noprof" {
		t.Fatalf("wrong generated maps symbols: %#v", profile.Symbols)
	}
}

func TestDefaultDeviceProfileIsPLK110(t *testing.T) {
	if DefaultProfileID != "oneplus-plk110-a16-b4999618-d05" {
		t.Fatalf("default profile id=%q", DefaultProfileID)
	}
	profile := DefaultDeviceProfile()
	if profile.ID != DefaultProfileID {
		t.Fatalf("default profile=%q", profile.ID)
	}
}

func TestFindDeviceProfileRejectsUnknownID(t *testing.T) {
	if _, found := FindDeviceProfile("oneplus-plk110-guessed"); found {
		t.Fatal("unknown profile matched")
	}
}
