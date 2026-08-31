package cmd

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"
	"time"

	"stackplz/user/config"
	"stackplz/user/event"
	"stackplz/user/kpm"
	"stackplz/user/module"
)

type recordingControlRunner struct {
	commands       []string
	requestPending bool
}

func (runner *recordingControlRunner) Control(_ context.Context, command string) (string, error) {
	runner.commands = append(runner.commands, command)
	switch command {
	case "status":
		if runner.requestPending {
			runner.requestPending = false
			return "status=0 version=1 state=ready profile=oneplus-plk110-a16-b4999618-d05 binding=none request=1 request_state=done request_status=0\n", nil
		}
		return "status=0 version=1 state=ready profile=oneplus-plk110-a16-b4999618-d05 binding=none\n", nil
	case "clear":
		runner.requestPending = true
		return "status=0 version=1 request=1\n", nil
	default:
		return "", errors.New("unexpected command: " + command)
	}
}

type resolverControlRunner struct {
	commands []string
	steps    []struct {
		command string
		output  string
		err     error
	}
}

func (runner *resolverControlRunner) Control(_ context.Context, command string) (string, error) {
	runner.commands = append(runner.commands, command)
	if len(runner.steps) == 0 {
		return "", errors.New("unexpected command: " + command)
	}
	step := runner.steps[0]
	runner.steps = runner.steps[1:]
	if step.command != command {
		return "", errors.New("got command " + command + ", want " + step.command)
	}
	return step.output, step.err
}

func TestValidateTracingOptionsRejectsUnsafeKPMCombinations(t *testing.T) {
	tests := []struct {
		name string
		edit func(*config.GlobalConfig)
		pids []uint32
		want string
	}{
		{name: "KPM needs numeric pid", edit: func(g *config.GlobalConfig) { g.TaskSource = "kpm" }, want: "numeric --pid"},
		{name: "KPM rejects name lookup", edit: func(g *config.GlobalConfig) { g.TaskSource, g.Name = "kpm", "hidden.app" }, pids: []uint32{7}, want: "does not support --name"},
		{name: "direct needs breakpoint", edit: func(g *config.GlobalConfig) { g.BrkBackend = "kpm-direct" }, pids: []uint32{7}, want: "requires --brk"},
		{name: "direct rejects multiple pids", edit: func(g *config.GlobalConfig) { g.BrkBackend, g.BrkAddr = "kpm-direct", "0x1000:x" }, pids: []uint32{7, 8}, want: "exactly one"},
		{name: "direct rejects name expansion", edit: func(g *config.GlobalConfig) { g.BrkBackend, g.BrkAddr, g.Name = "kpm-direct", "0x1000:x", "target.app" }, pids: []uint32{7}, want: "does not support --name"},
		{name: "zero breakpoint length", edit: func(g *config.GlobalConfig) { g.BrkAddr, g.BrkLen = "0x1000:x", 0 }, pids: []uint32{7}, want: "support [1, 8]"},
		{name: "unknown profile", edit: func(g *config.GlobalConfig) { g.TaskSource, g.KPMProfile = "kpm", "unknown" }, pids: []uint32{7}, want: "unknown KPM profile"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			options := config.NewGlobalConfig()
			test.edit(options)
			_, _, err := validateTracingOptions(options, test.pids)
			if err == nil || !strings.Contains(err.Error(), test.want) {
				t.Fatalf("error=%v want substring %q", err, test.want)
			}
		})
	}
}

func TestValidateTracingOptionsAllowsKPMMapsForLibraryBreakpoint(t *testing.T) {
	options := config.NewGlobalConfig()
	options.TaskSource = "kpm"
	options.BrkLib = "libtarget.so"
	if _, _, err := validateTracingOptions(options, []uint32{7}); err != nil {
		t.Fatalf("KPM maps-backed breakpoint was rejected: %v", err)
	}
}

func TestCacheKPMTargetMapsCompletesBeforePreparationClear(t *testing.T) {
	const profile = "oneplus-plk110-a16-b4999618-d05"
	const contentHex = "37303030303030302d373030303130303020722d787020302030303a30302030202f68696464656e2f6c69627461726765742e736f0a"
	runner := &resolverControlRunner{steps: []struct {
		command string
		output  string
		err     error
	}{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=bound generation=4 pid=31337 tgid=31337 uid=10234 start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc maps_supported=1 maps_state=task maps_snapshot=0 maps_size=0\n"},
		{command: "maps", output: "status=0 version=1 request=7\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=bound generation=4 pid=31337 tgid=31337 uid=10234 start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc maps_supported=1 maps_state=ready maps_snapshot=7 maps_size=54 request=7 request_state=done request_status=0\n"},
		{command: "maps-read snapshot=7 offset=0", output: "status=0 version=1 snapshot=7 offset=0 total=54 crc32=476647116 eof=1 data=" + contentHex + "\n"},
		{command: "clear", output: "status=0 version=1 request=8\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none maps_supported=1 maps_state=empty maps_snapshot=0 maps_size=0 request=8 request_state=done request_status=0\n"},
	}}
	client, _ := kpm.NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	errorsByPID := cacheKPMTargetMaps(context.Background(), client,
		[]ResolvedTarget{{PID: 31337, Source: TargetSourceKPM}}, "")
	if len(errorsByPID) != 0 {
		t.Fatalf("maps errors=%v", errorsByPID)
	}
	info, err := event.FindLibInMaps(31337, "libtarget.so")
	if err != nil || info.BaseAddr != 0x70000000 {
		t.Fatalf("cached library=%#v err=%v", info, err)
	}
	if err := clearKPMPreparationBinding(context.Background(), client,
		[]ResolvedTarget{{PID: 31337, Source: TargetSourceKPM}}); err != nil {
		t.Fatal(err)
	}
	want := []string{"status", "maps", "status", "maps-read snapshot=7 offset=0", "clear", "status"}
	if !reflect.DeepEqual(runner.commands, want) {
		t.Fatalf("commands=%v want=%v", runner.commands, want)
	}
}

func TestResolveRuntimePIDTargetsKPMNeverEnrichesProc(t *testing.T) {
	resolver := &fakeTargetResolver{kpmIdentity: kpm.Identity{PID: 31337, TGID: 31337, UID: 10234}}
	procEnrichCalls := 0
	got, err := resolveRuntimePIDTargets(context.Background(), resolver, TargetSourceKPM, []uint32{31337}, nil, func(ResolvedTarget) error {
		procEnrichCalls++
		return errors.New("must not enrich proc")
	})
	if err != nil {
		t.Fatal(err)
	}
	if resolver.procCalls != 0 || procEnrichCalls != 0 || got[0].UID != 10234 {
		t.Fatalf("proc=%d enrich=%d targets=%#v", resolver.procCalls, procEnrichCalls, got)
	}
}

func TestResolveRuntimePIDTargetsEnrichesOnlySelectedProcTargets(t *testing.T) {
	resolver := &fakeTargetResolver{procUID: 10001}
	var enriched []uint32
	got, err := resolveRuntimePIDTargets(context.Background(), resolver, TargetSourceAuto, []uint32{9}, nil, func(target ResolvedTarget) error {
		enriched = append(enriched, target.PID)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(enriched, []uint32{9}) || got[0].Source != TargetSourceProc {
		t.Fatalf("enriched=%v targets=%#v", enriched, got)
	}
}

func TestSelectModuleNamesUsesDirectKPMOnlyWhenSelected(t *testing.T) {
	conf := config.NewModuleConfig()
	conf.BrkAddr = 0x1000
	conf.BrkBackend = "kpm-direct"
	if got, err := selectModuleNames(conf); err != nil || !reflect.DeepEqual(got, []string{module.MODULE_NAME_KPM_BRK}) {
		t.Fatalf("direct modules=%v error=%v", got, err)
	}
	conf.BrkBackend = "perf"
	if got, err := selectModuleNames(conf); err != nil || !reflect.DeepEqual(got, []string{module.MODULE_NAME_BRK}) {
		t.Fatalf("perf modules=%v error=%v", got, err)
	}
}

func TestClearKPMPreparationBindingOnlyAfterKPMResolution(t *testing.T) {
	runner := &recordingControlRunner{}
	client, err := kpm.NewClient(runner, "oneplus-plk110-a16-b4999618-d05")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := clearKPMPreparationBinding(context.Background(), client, []ResolvedTarget{{PID: 7, Source: TargetSourceProc}}); err != nil {
		t.Fatal(err)
	}
	if err := clearKPMPreparationBinding(context.Background(), client, []ResolvedTarget{{PID: 8, Source: TargetSourceKPM}}); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(runner.commands, []string{"status", "clear", "status"}) {
		t.Fatalf("commands=%v", runner.commands)
	}
}

func TestLiveTargetResolverClearsFailedBinding(t *testing.T) {
	const profile = "oneplus-plk110-a16-b4999618-d05"
	runner := &resolverControlRunner{steps: []struct {
		command string
		output  string
		err     error
	}{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none\n"},
		{command: "bind pid=31337 mode=either", output: "status=0 version=1 state=pending generation=4\n"},
		{command: "status", output: "status=-5 version=1 reason=observer_error\n"},
		{command: "clear", output: "status=0 version=1 request=2\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none request=2 request_state=done request_status=0\n"},
	}}
	client, err := kpm.NewClient(runner, profile)
	if err != nil {
		t.Fatal(err)
	}
	resolver := &liveTargetResolver{client: client, bindTimeout: time.Second, pollInterval: time.Millisecond}
	if _, err := resolver.IdentityFromKPM(context.Background(), 31337); err == nil {
		t.Fatal("failed observation unexpectedly succeeded")
	}
	want := []string{"status", "bind pid=31337 mode=either", "status", "clear", "status"}
	if !reflect.DeepEqual(runner.commands, want) {
		t.Fatalf("commands=%v want=%v", runner.commands, want)
	}
}
