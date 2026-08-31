package kpm

import (
	"context"
	"encoding/hex"
	"errors"
	"reflect"
	"sync"
	"testing"
	"time"
)

type scriptedStep struct {
	command string
	output  string
	err     error
}

type scriptedRunner struct {
	mu       sync.Mutex
	steps    []scriptedStep
	commands []string
}

func (runner *scriptedRunner) Control(_ context.Context, command string) (string, error) {
	runner.mu.Lock()
	defer runner.mu.Unlock()
	runner.commands = append(runner.commands, command)
	if len(runner.steps) == 0 {
		return "", errors.New("unexpected control call")
	}
	step := runner.steps[0]
	runner.steps = runner.steps[1:]
	if step.command != command {
		return "", errors.New("unexpected command: " + command)
	}
	return step.output, step.err
}

func TestClientRejectsProfileMismatchBeforeMutation(t *testing.T) {
	runner := &scriptedRunner{steps: []scriptedStep{{
		command: "status",
		output:  "status=0 version=1 state=ready profile=wrong-profile\n",
	}}}
	client, err := NewClient(runner, "oneplus-plk110-a16-b4999618-d05")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Status(context.Background()); !errors.Is(err, ErrProfileMismatch) {
		t.Fatalf("status error=%v", err)
	}
	if _, err := client.Bind(context.Background(), BindingRequest{PID: 7, Mode: BindPID}); !errors.Is(err, ErrNotReady) {
		t.Fatalf("bind error=%v", err)
	}
	if err := client.Clear(context.Background()); !errors.Is(err, ErrNotReady) {
		t.Fatalf("clear after profile mismatch error=%v", err)
	}
	if len(runner.commands) != 1 {
		t.Fatalf("mutation reached runner: %q", runner.commands)
	}
}

func TestClientWaitBoundReturnsSchedulerIdentity(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none\n"},
		{command: "bind pid=31337 mode=pid", output: "status=0 version=1 state=pending generation=4\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=pending generation=4\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=bound generation=4 pid=31337 tgid=31300 uid=10234 start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc\n"},
	}}
	client, err := NewClient(runner, profile)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	generation, err := client.Bind(context.Background(), BindingRequest{PID: 31337, Mode: BindPID})
	if err != nil || generation != 4 {
		t.Fatalf("bind generation=%d error=%v", generation, err)
	}
	identity, err := client.WaitBound(context.Background(), time.Microsecond)
	if err != nil {
		t.Fatal(err)
	}
	want := Identity{Generation: 4, TaskCookie: 0xabc, PID: 31337, TGID: 31300, UID: 10234, StartTime: 111, StartBootTime: 222, Comm: "target.proc"}
	if !reflect.DeepEqual(identity, want) {
		t.Fatalf("identity=%#v want=%#v", identity, want)
	}
}

func TestIdentityFromFieldsDecodesCommHexWithSpaces(t *testing.T) {
	comm := make([]byte, CommBytes)
	copy(comm, "Profile Saver")
	identity, err := identityFromFields(map[string]string{
		"generation":     "4",
		"task_cookie":    "0xabc",
		"pid":            "31337",
		"tgid":           "31300",
		"uid":            "10234",
		"start_time":     "111",
		"start_boottime": "222",
		"comm_hex":       hex.EncodeToString(comm),
	})
	if err != nil {
		t.Fatal(err)
	}
	if identity.Comm != "Profile Saver" {
		t.Fatalf("comm=%q", identity.Comm)
	}
}

func TestClientConfiguresAndCleansBreakpoint(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + "\n"},
		{command: "break id=7 kind=x addr=0x7abc1000 len=4 mode=once", output: "status=0 version=1 configured=7\n"},
		{command: "enable id=7", output: "status=0 version=1 request=1\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " request=1 request_state=pending request_status=-115\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " request=1 request_state=done request_status=0\n"},
		{command: "disable id=7", output: "status=0 version=1 request=2\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " request=2 request_state=done request_status=0\n"},
		{command: "clear", output: "status=0 version=1 request=3\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " request=3 request_state=done request_status=0\n"},
	}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	bp := Breakpoint{ID: 7, Kind: BreakExecute, Address: 0x7abc1000, Length: 4, Mode: BreakOnce}
	if err := client.ConfigureBreakpoint(context.Background(), bp); err != nil {
		t.Fatal(err)
	}
	if err := client.Enable(context.Background(), 7); err != nil {
		t.Fatal(err)
	}
	if err := client.Disable(context.Background(), 7); err != nil {
		t.Fatal(err)
	}
	if err := client.Clear(context.Background()); err != nil {
		t.Fatal(err)
	}
}

func TestClientReportsAsyncWorkerFailure(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + "\n"},
		{command: "enable id=7", output: "status=0 version=1 request=9\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " request=9 request_state=done request_status=-16\n"},
	}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	err := client.Enable(context.Background(), 7)
	var asyncErr *AsyncRequestError
	if !errors.As(err, &asyncErr) || asyncErr.RequestID != 9 || asyncErr.Status != -16 {
		t.Fatalf("async error=%v", err)
	}
}

func TestClientAcceptsIdempotentZeroRequest(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + "\n"},
		{command: "disable id=7", output: "status=0 version=1 request=0\n"},
	}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := client.Disable(context.Background(), 7); err != nil {
		t.Fatal(err)
	}
	if len(runner.steps) != 0 {
		t.Fatalf("unexpected status polling: %#v", runner.steps)
	}
}

func TestClientRejectsMismatchedAsyncRequest(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + "\n"},
		{command: "clear", output: "status=0 version=1 request=4\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " request=5 request_state=done request_status=0\n"},
	}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := client.Clear(context.Background()); !errors.Is(err, ErrMalformedResponse) {
		t.Fatalf("clear error=%v", err)
	}
}

func TestClientClearRemainsAvailableAfterTransientStatusError(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	transient := errors.New("status transport failed")
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none\n"},
		{command: "status", err: transient},
		{command: "clear", output: "status=0 version=1 request=1\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=none request=1 request_state=done request_status=0\n"},
	}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	if _, err := client.Status(context.Background()); !errors.Is(err, transient) {
		t.Fatalf("status error=%v", err)
	}
	if err := client.Clear(context.Background()); err != nil {
		t.Fatalf("cleanup was blocked after validated profile: %v", err)
	}
}

func TestClientPollDecodesOneEvent(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	encoded := hex.EncodeToString(eventFixture())
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + "\n"},
		{command: "poll after=8", output: "status=0 version=1 event=" + encoded + "\n"},
	}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	event, found, err := client.Poll(context.Background(), 8)
	if err != nil || !found || event.PID != 31337 {
		t.Fatalf("event=%#v found=%t error=%v", event, found, err)
	}
}

func TestClientSnapshotMapsAssemblesChunksAndChecksCRC(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	runner := &scriptedRunner{steps: []scriptedStep{
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=bound generation=4 pid=31337 tgid=31337 uid=10234 start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc maps_supported=1 maps_state=task maps_snapshot=0 maps_size=0\n"},
		{command: "maps", output: "status=0 version=1 request=7\n"},
		{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " binding=bound generation=4 pid=31337 tgid=31337 uid=10234 start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc maps_supported=1 maps_state=ready maps_snapshot=7 maps_size=6 request=7 request_state=done request_status=0\n"},
		{command: "maps-read snapshot=7 offset=0", output: "status=0 version=1 snapshot=7 offset=0 total=6 crc32=1267612143 eof=0 data=616263\n"},
		{command: "maps-read snapshot=7 offset=3", output: "status=0 version=1 snapshot=7 offset=3 total=6 crc32=1267612143 eof=1 data=646566\n"},
	}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	content, err := client.SnapshotMaps(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if string(content) != "abcdef" {
		t.Fatalf("content=%q", content)
	}
	if len(runner.steps) != 0 {
		t.Fatalf("unused runner steps: %#v", runner.steps)
	}
}

func TestClientSnapshotMapsStopsWhenCapabilityIsUnsupported(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	runner := &scriptedRunner{steps: []scriptedStep{{
		command: "status",
		output:  "status=0 version=1 state=ready profile=" + profile + " binding=bound generation=4 pid=31337 tgid=31337 uid=10234 start_time=111 start_boottime=222 task_cookie=0xabc comm=target.proc maps_supported=0 maps_state=unsupported maps_snapshot=0 maps_size=0\n",
	}}}
	client, _ := NewClient(runner, profile)
	if _, err := client.Status(context.Background()); err != nil {
		t.Fatal(err)
	}
	if _, err := client.SnapshotMaps(context.Background()); !errors.Is(err, ErrMapsUnsupported) {
		t.Fatalf("snapshot error=%v", err)
	}
	if len(runner.commands) != 1 {
		t.Fatalf("unsupported maps reached mutation: %q", runner.commands)
	}
}

func TestClientSnapshotMapsRejectsBrokenChunkContracts(t *testing.T) {
	profile := "oneplus-plk110-a16-b4999618-d05"
	tests := []struct {
		name   string
		output string
		want   error
	}{
		{name: "stale snapshot", output: "status=0 version=1 snapshot=8 offset=0 total=3 crc32=891568578 eof=1 data=616263\n", want: ErrMalformedResponse},
		{name: "wrong offset", output: "status=0 version=1 snapshot=7 offset=1 total=3 crc32=891568578 eof=1 data=616263\n", want: ErrMalformedResponse},
		{name: "odd hex", output: "status=0 version=1 snapshot=7 offset=0 total=3 crc32=891568578 eof=1 data=61626\n", want: ErrMalformedResponse},
		{name: "early eof", output: "status=0 version=1 snapshot=7 offset=0 total=4 crc32=891568578 eof=1 data=616263\n", want: ErrMalformedResponse},
		{name: "wrong crc", output: "status=0 version=1 snapshot=7 offset=0 total=3 crc32=1 eof=1 data=616263\n", want: ErrMapsChecksum},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			runner := &scriptedRunner{steps: []scriptedStep{
				{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " maps_supported=1 maps_state=task maps_snapshot=0 maps_size=0\n"},
				{command: "maps", output: "status=0 version=1 request=7\n"},
				{command: "status", output: "status=0 version=1 state=ready profile=" + profile + " maps_supported=1 maps_state=ready maps_snapshot=7 maps_size=3 request=7 request_state=done request_status=0\n"},
				{command: "maps-read snapshot=7 offset=0", output: test.output},
			}}
			client, _ := NewClient(runner, profile)
			if _, err := client.Status(context.Background()); err != nil {
				t.Fatal(err)
			}
			if _, err := client.SnapshotMaps(context.Background()); !errors.Is(err, test.want) {
				t.Fatalf("snapshot error=%v want=%v", err, test.want)
			}
		})
	}
}
