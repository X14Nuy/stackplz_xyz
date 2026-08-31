package module

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"strings"
	"sync"
	"time"

	"stackplz/user/config"
	"stackplz/user/event"
	"stackplz/user/kpm"
	"stackplz/user/util"

	"github.com/cilium/ebpf"
	"github.com/cilium/ebpf/perf"
)

const (
	kpmBreakpointID     uint32 = 1
	defaultPollInterval        = 10 * time.Millisecond
	defaultKPMTimeout          = 10 * time.Second
)

type KPMBRK struct {
	name string

	ctx    context.Context
	cancel context.CancelFunc
	logger *log.Logger
	mconf  *config.ModuleConfig
	runner kpm.Runner
	client *kpm.Client

	pollInterval time.Duration
	pollWG       sync.WaitGroup

	mu              sync.Mutex
	started         bool
	mutationStarted bool
	pollErr         error

	cleanupOnce sync.Once
	cleanupErr  error
	closeOnce   sync.Once
	closeErr    error
}

func newKPMBRKForTest(runner kpm.Runner) *KPMBRK {
	return &KPMBRK{name: MODULE_NAME_KPM_BRK, runner: runner, pollInterval: time.Millisecond}
}

func (mod *KPMBRK) Init(parent context.Context, logger *log.Logger, conf config.IConfig) error {
	mconf, ok := conf.(*config.ModuleConfig)
	if !ok || mconf == nil {
		return errors.New("KPM breakpoint module requires ModuleConfig")
	}
	if parent == nil {
		parent = context.Background()
	}
	if logger == nil {
		logger = log.New(io.Discard, "", 0)
	}
	if mod.runner == nil {
		runner, err := kpm.NewExecRunner(mconf.KPMControl, mconf.KPMModule)
		if err != nil {
			return fmt.Errorf("create KPM control runner: %w", err)
		}
		mod.runner = runner
	}
	client, err := kpm.NewClient(mod.runner, mconf.KPMProfile)
	if err != nil {
		return fmt.Errorf("create KPM client: %w", err)
	}
	mod.ctx, mod.cancel = context.WithCancel(parent)
	mod.logger = logger
	mod.mconf = mconf
	mod.client = client
	if mod.name == "" {
		mod.name = MODULE_NAME_KPM_BRK
	}
	if mod.pollInterval <= 0 {
		mod.pollInterval = defaultPollInterval
	}
	return nil
}

func (mod *KPMBRK) Name() string {
	return mod.name
}

func (mod *KPMBRK) Clone() IModule {
	return &KPMBRK{name: mod.name, pollInterval: defaultPollInterval}
}

func (mod *KPMBRK) GetConf() config.IConfig {
	return mod.mconf
}

func (mod *KPMBRK) SetChild(IModule) {}

func (mod *KPMBRK) PrePare(*ebpf.Map, perf.Record) (event.IEventStruct, error) {
	return nil, errors.New("KPM breakpoint module does not consume perf records")
}

func (mod *KPMBRK) Events() []*ebpf.Map {
	return nil
}

func (mod *KPMBRK) DecodeFun(*ebpf.Map) (event.IEventStruct, bool) {
	return nil, false
}

func (mod *KPMBRK) Start() error {
	mod.mu.Lock()
	if mod.started {
		mod.mu.Unlock()
		return nil
	}
	if mod.client == nil || mod.mconf == nil || mod.ctx == nil {
		mod.mu.Unlock()
		return errors.New("KPM breakpoint module is not initialized")
	}
	mod.mu.Unlock()

	pid, uid, breakpoint, timeout, err := mod.startConfiguration()
	if err != nil {
		return err
	}
	if _, err := mod.client.Status(mod.ctx); err != nil {
		return fmt.Errorf("KPM status: %w", err)
	}

	mod.mu.Lock()
	mod.mutationStarted = true
	mod.mu.Unlock()
	if _, err := mod.client.Bind(mod.ctx, kpm.BindingRequest{PID: pid, Mode: kpm.BindEither, UID: uid}); err != nil {
		return mod.failStart(fmt.Errorf("KPM bind pid %d: %w", pid, err))
	}
	bindCtx, cancel := context.WithTimeout(mod.ctx, timeout)
	identity, err := mod.client.WaitBound(bindCtx, mod.pollInterval)
	cancel()
	if err != nil {
		return mod.failStart(fmt.Errorf("wait for scheduler identity pid %d: %w", pid, err))
	}
	if err := mod.client.ConfigureBreakpoint(mod.ctx, breakpoint); err != nil {
		return mod.failStart(fmt.Errorf("configure direct breakpoint: %w", err))
	}
	if err := mod.client.Enable(mod.ctx, kpmBreakpointID); err != nil {
		return mod.failStart(fmt.Errorf("enable direct breakpoint: %w", err))
	}

	mod.mu.Lock()
	mod.started = true
	mod.mu.Unlock()
	mod.logger.Printf("kpm_bound pid=%d tgid=%d uid=%d comm=%s generation=%d", identity.PID, identity.TGID, identity.UID, identity.Comm, identity.Generation)
	return nil
}

func (mod *KPMBRK) startConfiguration() (uint32, *uint32, kpm.Breakpoint, time.Duration, error) {
	if len(mod.mconf.PidWhitelist) != 1 || mod.mconf.PidWhitelist[0] == 0 {
		return 0, nil, kpm.Breakpoint{}, 0, errors.New("kpm-direct requires exactly one nonzero --pid")
	}
	var uid *uint32
	if len(mod.mconf.UidWhitelist) > 1 {
		return 0, nil, kpm.Breakpoint{}, 0, errors.New("kpm-direct accepts at most one target UID")
	}
	if len(mod.mconf.UidWhitelist) == 1 {
		value := mod.mconf.UidWhitelist[0]
		uid = &value
	}
	kind, err := kpmBreakKind(mod.mconf.BrkType)
	if err != nil {
		return 0, nil, kpm.Breakpoint{}, 0, err
	}
	mode := kpm.BreakMode(mod.mconf.BrkMode)
	if mode != kpm.BreakOnce && mode != kpm.BreakRepeat {
		return 0, nil, kpm.Breakpoint{}, 0, fmt.Errorf("invalid KPM breakpoint mode %q", mod.mconf.BrkMode)
	}
	if mod.mconf.BrkLen > 255 {
		return 0, nil, kpm.Breakpoint{}, 0, fmt.Errorf("invalid KPM breakpoint length %d", mod.mconf.BrkLen)
	}
	breakpoint := kpm.Breakpoint{
		ID:      kpmBreakpointID,
		Kind:    kind,
		Address: mod.mconf.BrkAddr,
		Length:  uint8(mod.mconf.BrkLen),
		Mode:    mode,
	}
	if _, err := kpm.BreakCommand(breakpoint); err != nil {
		return 0, nil, kpm.Breakpoint{}, 0, fmt.Errorf("invalid direct breakpoint: %w", err)
	}
	timeout := mod.mconf.KPMBindTimeout
	if timeout <= 0 {
		timeout = defaultKPMTimeout
	}
	return mod.mconf.PidWhitelist[0], uid, breakpoint, timeout, nil
}

func kpmBreakKind(value uint32) (kpm.BreakKind, error) {
	switch value {
	case util.HW_BREAKPOINT_X:
		return kpm.BreakExecute, nil
	case util.HW_BREAKPOINT_R:
		return kpm.BreakRead, nil
	case util.HW_BREAKPOINT_W:
		return kpm.BreakWrite, nil
	case util.HW_BREAKPOINT_RW:
		return kpm.BreakReadWrite, nil
	default:
		return "", fmt.Errorf("unsupported breakpoint type %d", value)
	}
}

func (mod *KPMBRK) failStart(startErr error) error {
	cleanupErr := mod.cleanup()
	if cleanupErr == nil {
		return startErr
	}
	return multiCauseError{prefix: startErr.Error(), causes: []error{startErr, cleanupErr}}
}

func (mod *KPMBRK) Run() error {
	if err := mod.Start(); err != nil {
		return err
	}
	mod.pollWG.Add(1)
	go mod.poll()
	return nil
}

func (mod *KPMBRK) poll() {
	defer mod.pollWG.Done()
	var after uint64
	for {
		select {
		case <-mod.ctx.Done():
			return
		default:
		}
		hit, found, err := mod.client.Poll(mod.ctx, after)
		if err != nil {
			/* CommandContext may surface SIGKILL instead of context.Canceled. */
			if mod.ctx.Err() == nil && !errors.Is(err, context.Canceled) {
				mod.mu.Lock()
				mod.pollErr = err
				mod.mu.Unlock()
				mod.logger.Printf("kpm_poll_error error=%v", err)
			}
			return
		}
		if !found {
			if !waitContext(mod.ctx, mod.pollInterval) {
				return
			}
			continue
		}
		if hit.Sequence <= after {
			mod.logger.Printf("kpm_poll_error error=non-increasing sequence got=%d after=%d", hit.Sequence, after)
			return
		}
		after = hit.Sequence
		mod.logHit(hit)
	}
}

func waitContext(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

func (mod *KPMBRK) logHit(hit kpm.Event) {
	if mod.mconf.FmtJson {
		encoded, err := json.Marshal(hit)
		if err != nil {
			mod.logger.Printf("kpm_event_encode_error error=%v", err)
			return
		}
		mod.logger.Print(string(encoded))
		return
	}
	var text strings.Builder
	fmt.Fprintf(&text, "kpm_hit seq=%d time=%d cpu=%d pid=%d tgid=%d uid=%d comm=%s kind=%d slot=%d requested=0x%x observed=0x%x value=0x%x pc=0x%x sp=0x%x pstate=0x%x",
		hit.Sequence, hit.Timestamp, hit.CPU, hit.PID, hit.TGID, hit.UID, hit.Comm, hit.SlotKind, hit.SlotIndex,
		hit.RequestedAddress, hit.ObservedAddress, hit.Value, hit.Registers.PC, hit.Registers.SP, hit.Registers.PState)
	for index, value := range hit.Registers.X {
		fmt.Fprintf(&text, " x%d=0x%x", index, value)
	}
	mod.logger.Print(text.String())
}

func (mod *KPMBRK) Stop() error {
	return mod.Close()
}

func (mod *KPMBRK) Close() error {
	mod.closeOnce.Do(func() {
		if mod.cancel != nil {
			mod.cancel()
		}
		mod.pollWG.Wait()
		mod.closeErr = mod.cleanup()
	})
	return mod.closeErr
}

func (mod *KPMBRK) cleanup() error {
	mod.cleanupOnce.Do(func() {
		mod.mu.Lock()
		shouldClean := mod.mutationStarted
		mod.mu.Unlock()
		if !shouldClean || mod.client == nil {
			return
		}
		timeout := defaultKPMTimeout
		if mod.mconf != nil && mod.mconf.KPMBindTimeout > 0 {
			timeout = mod.mconf.KPMBindTimeout
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()
		var causes []error
		if err := mod.client.Disable(ctx, kpmBreakpointID); err != nil {
			causes = append(causes, fmt.Errorf("disable KPM breakpoint: %w", err))
		}
		if err := mod.client.Clear(ctx); err != nil {
			causes = append(causes, fmt.Errorf("clear KPM state: %w", err))
		}
		if len(causes) != 0 {
			mod.cleanupErr = multiCauseError{prefix: "KPM cleanup failed", causes: causes}
		}
	})
	return mod.cleanupErr
}

type multiCauseError struct {
	prefix string
	causes []error
}

func (err multiCauseError) Error() string {
	parts := make([]string, 0, len(err.causes))
	for _, cause := range err.causes {
		parts = append(parts, cause.Error())
	}
	return err.prefix + ": " + strings.Join(parts, "; ")
}

func (err multiCauseError) Is(target error) bool {
	for _, cause := range err.causes {
		if errors.Is(cause, target) {
			return true
		}
	}
	return false
}

func init() {
	Register(&KPMBRK{name: MODULE_NAME_KPM_BRK, pollInterval: defaultPollInterval})
}
