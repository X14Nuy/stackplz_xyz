package module

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"hash/crc32"
	"log"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"stackplz/user/config"
	"stackplz/user/kpm"
	"stackplz/user/util"
)

type moduleControlStep struct {
	command       string
	output        string
	err           error
	notify        chan struct{}
	waitForCancel bool
}

type moduleScriptRunner struct {
	mu       sync.Mutex
	steps    []moduleControlStep
	commands []string
}

func (runner *moduleScriptRunner) Control(ctx context.Context, command string) (string, error) {
	runner.mu.Lock()
	defer runner.mu.Unlock()
	runner.commands = append(runner.commands, command)
	if len(runner.steps) == 0 {
		return "", errors.New("unexpected control call: " + command)
	}
	step := runner.steps[0]
	runner.steps = runner.steps[1:]
	if step.command != command {
		return "", errors.New("unexpected command: " + command + ", want " + step.command)
	}
	if step.notify != nil {
		close(step.notify)
	}
	if step.waitForCancel {
		<-ctx.Done()
	}
	return step.output, step.err
}

func (runner *moduleScriptRunner) commandSnapshot() []string {
	runner.mu.Lock()
	defer runner.mu.Unlock()
	return append([]string(nil), runner.commands...)
}

func readyModuleSteps() []moduleControlStep {
	const profile = "oneplus-plk110-a16-b4999618-d05"
	return []moduleControlStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none\n"},
		{command: "bind pid=31337 mode=either uid=10234", output: "status=0 version=1 state=pending generation=4\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=bound generation=4 pid=31337 tgid=31337 uid=10234 start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc\n"},
		{command: "break id=1 kind=x addr=0x7abc1000 len=4 mode=once", output: "status=0 version=1 configured=1\n"},
		{command: "enable id=1", output: "status=0 version=1 request=1\n"},
		{command: "status", output: boundModuleRequestStatus(1)},
	}
}

func boundModuleRequestStatus(request uint64) string {
	const profile = "oneplus-plk110-a16-b4999618-d05"
	return "status=0 version=1 state=ready profile=" + profile +
		" binding=bound generation=4 pid=31337 tgid=31337 uid=10234" +
		" start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc" +
		" request=" + strconv.FormatUint(request, 10) +
		" request_state=done request_status=0\n"
}

func cleanupModuleSteps() []moduleControlStep {
	const profile = "oneplus-plk110-a16-b4999618-d05"
	return []moduleControlStep{
		{command: "disable id=1", output: "status=0 version=1 request=2\n"},
		{command: "status", output: boundModuleRequestStatus(2)},
		{command: "clear", output: "status=0 version=1 request=3\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none request=3 request_state=done request_status=0\n"},
	}
}

func testKPMModuleConfig() *config.ModuleConfig {
	conf := config.NewModuleConfig()
	conf.KPMProfile = "oneplus-plk110-a16-b4999618-d05"
	conf.KPMControl = "/test/kpatch"
	conf.KPMModule = "stackplz-kpm"
	conf.KPMBindTimeout = time.Second
	conf.PidWhitelist = []uint32{31337}
	conf.UidWhitelist = []uint32{10234}
	conf.BrkAddr = 0x7abc1000
	conf.BrkLen = 4
	conf.BrkType = util.HW_BREAKPOINT_X
	conf.BrkMode = "once"
	return conf
}

func initTestKPMModule(t *testing.T, runner kpm.Runner, output *bytes.Buffer) *KPMBRK {
	t.Helper()
	mod := newKPMBRKForTest(runner)
	if err := mod.Init(context.Background(), log.New(output, "", 0), testKPMModuleConfig()); err != nil {
		t.Fatal(err)
	}
	return mod
}

func TestKPMBRKStartAndCloseCommandOrder(t *testing.T) {
	steps := append(readyModuleSteps(), cleanupModuleSteps()...)
	runner := &moduleScriptRunner{steps: steps}
	mod := initTestKPMModule(t, runner, &bytes.Buffer{})
	if err := mod.Start(); err != nil {
		t.Fatal(err)
	}
	if err := mod.Close(); err != nil {
		t.Fatal(err)
	}
	want := []string{
		"status",
		"bind pid=31337 mode=either uid=10234",
		"status",
		"break id=1 kind=x addr=0x7abc1000 len=4 mode=once",
		"enable id=1",
		"status",
		"disable id=1",
		"status",
		"clear",
		"status",
	}
	if got := runner.commandSnapshot(); !reflect.DeepEqual(got, want) {
		t.Fatalf("commands=%q want=%q", got, want)
	}
}

func TestKPMBRKRunPollsDecodedEventAndCleansOnce(t *testing.T) {
	pollDone := make(chan struct{})
	encoded := testKPMEventHex(77)
	steps := append(readyModuleSteps(),
		moduleControlStep{command: "poll after=0", output: "status=0 version=1 event=" + encoded + "\n"},
		moduleControlStep{command: "poll after=77", err: errors.New("poll stopped"), notify: pollDone},
	)
	steps = append(steps, cleanupModuleSteps()...)
	runner := &moduleScriptRunner{steps: steps}
	var output bytes.Buffer
	mod := initTestKPMModule(t, runner, &output)
	if err := mod.Run(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-pollDone:
	case <-time.After(2 * time.Second):
		t.Fatal("poll loop did not run")
	}
	if err := mod.Close(); err != nil {
		t.Fatal(err)
	}
	if err := mod.Close(); err != nil {
		t.Fatal(err)
	}
	if text := output.String(); !strings.Contains(text, "kpm_hit seq=77") || !strings.Contains(text, "pid=31337") {
		t.Fatalf("decoded event not logged: %q", text)
	}
	wantSuffix := []string{"disable id=1", "status", "clear", "status"}
	commands := runner.commandSnapshot()
	if len(commands) < len(wantSuffix) ||
		!reflect.DeepEqual(commands[len(commands)-len(wantSuffix):], wantSuffix) {
		t.Fatalf("cleanup suffix=%q", commands)
	}
	for _, command := range []string{"disable id=1", "clear"} {
		count := 0
		for _, got := range commands {
			if got == command {
				count++
			}
		}
		if count != 1 {
			t.Fatalf("command %q count=%d", command, count)
		}
	}
}

func TestKPMBRKCloseDoesNotReportCanceledControlProcess(t *testing.T) {
	pollStarted := make(chan struct{})
	steps := append(readyModuleSteps(), moduleControlStep{
		command:       "poll after=0",
		err:           errors.New("control process: signal: killed"),
		notify:        pollStarted,
		waitForCancel: true,
	})
	steps = append(steps, cleanupModuleSteps()...)
	runner := &moduleScriptRunner{steps: steps}
	var output bytes.Buffer
	mod := initTestKPMModule(t, runner, &output)
	if err := mod.Run(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-pollStarted:
	case <-time.After(2 * time.Second):
		t.Fatal("poll loop did not start")
	}
	if err := mod.Close(); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(output.String(), "kpm_poll_error") {
		t.Fatalf("canceled poll was reported as an error: %q", output.String())
	}
	mod.mu.Lock()
	pollErr := mod.pollErr
	mod.mu.Unlock()
	if pollErr != nil {
		t.Fatalf("canceled poll retained error: %v", pollErr)
	}
}

func TestKPMBRKStartFailureRollsBack(t *testing.T) {
	enableErr := errors.New("enable rejected")
	steps := readyModuleSteps()
	steps[4].err = enableErr
	steps = steps[:5]
	steps = append(steps,
		moduleControlStep{command: "disable id=1", output: "status=0 version=1 request=0\n"},
		moduleControlStep{command: "clear", output: "status=0 version=1 request=2\n"},
		moduleControlStep{command: "status", output: "status=0 version=1 state=ready profile=oneplus-plk110-a16-b4999618-d05 binding=none request=2 request_state=done request_status=0\n"},
	)
	runner := &moduleScriptRunner{steps: steps}
	mod := initTestKPMModule(t, runner, &bytes.Buffer{})
	if err := mod.Start(); !errors.Is(err, enableErr) {
		t.Fatalf("start error=%v", err)
	}
	if err := mod.Close(); err != nil {
		t.Fatal(err)
	}
	commands := runner.commandSnapshot()
	wantSuffix := []string{"disable id=1", "clear", "status"}
	if len(commands) < len(wantSuffix) ||
		!reflect.DeepEqual(commands[len(commands)-len(wantSuffix):], wantSuffix) {
		t.Fatalf("rollback=%q", commands)
	}
}

func TestKPMBRKRejectsInvalidBreakpointBeforeControl(t *testing.T) {
	runner := &moduleScriptRunner{}
	conf := testKPMModuleConfig()
	conf.BrkLen = 1
	mod := newKPMBRKForTest(runner)
	if err := mod.Init(context.Background(), log.New(&bytes.Buffer{}, "", 0), conf); err != nil {
		t.Fatal(err)
	}
	if err := mod.Start(); !errors.Is(err, kpm.ErrInvalidLength) {
		t.Fatalf("start error=%v", err)
	}
	if commands := runner.commandSnapshot(); len(commands) != 0 {
		t.Fatalf("invalid config reached control runner: %v", commands)
	}
}

func TestKPMBRKCloseAttemptsBothCleanupCommands(t *testing.T) {
	disableErr := errors.New("disable failed")
	clearErr := errors.New("clear failed")
	steps := append(readyModuleSteps(),
		moduleControlStep{command: "disable id=1", err: disableErr},
		moduleControlStep{command: "clear", err: clearErr},
	)
	runner := &moduleScriptRunner{steps: steps}
	mod := initTestKPMModule(t, runner, &bytes.Buffer{})
	if err := mod.Start(); err != nil {
		t.Fatal(err)
	}
	err := mod.Close()
	if !errors.Is(err, disableErr) || !errors.Is(err, clearErr) {
		t.Fatalf("cleanup error=%v", err)
	}
	commands := runner.commandSnapshot()
	if !reflect.DeepEqual(commands[len(commands)-2:], []string{"disable id=1", "clear"}) {
		t.Fatalf("cleanup=%q", commands)
	}
}

func TestKPMBRKIsRegistered(t *testing.T) {
	mod := GetModuleByName(MODULE_NAME_KPM_BRK)
	if mod == nil || mod.Name() != MODULE_NAME_KPM_BRK {
		t.Fatalf("registered module=%v", mod)
	}
}

func testKPMEventHex(sequence uint64) string {
	raw := make([]byte, kpm.EventWireSize)
	put16 := func(offset int, value uint16) { binary.LittleEndian.PutUint16(raw[offset:], value) }
	put32 := func(offset int, value uint32) { binary.LittleEndian.PutUint32(raw[offset:], value) }
	put64 := func(offset int, value uint64) { binary.LittleEndian.PutUint64(raw[offset:], value) }
	put32(0, kpm.EventMagic)
	put16(4, kpm.ProtocolVersion)
	put16(6, kpm.EventWireSize)
	put16(8, 1)
	put32(12, 2)
	put64(16, sequence)
	put64(24, 998877)
	put32(32, 4)
	put32(36, 1)
	put64(40, 4)
	put64(48, 0xabc)
	put32(56, 31337)
	put32(60, 31337)
	put32(64, 10234)
	copy(raw[88:104], []byte("target.proc"))
	put64(112, 0x7abc1000)
	put64(120, 0x7abc1000)
	put64(408, 0x7abc1000)
	put32(428, crc32.ChecksumIEEE(raw[:428]))
	return hex.EncodeToString(raw)
}
