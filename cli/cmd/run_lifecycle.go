package cmd

import (
	"context"
	"errors"
	"fmt"
	"log"
	"strings"

	"stackplz/user/config"
	"stackplz/user/module"
)

type runModule interface {
	Name() string
	Init(context.Context, *log.Logger, config.IConfig) error
	Run() error
	Close() error
}

type runModuleFactory func(name string) (runModule, error)

func registeredModule(name string) (runModule, error) {
	mod := module.GetModuleByName(name)
	if mod == nil {
		return nil, fmt.Errorf("module %q is not registered", name)
	}
	return mod, nil
}

func startModules(ctx context.Context, logger *log.Logger, conf config.IConfig, names []string, factory runModuleFactory) ([]runModule, error) {
	if factory == nil {
		return nil, errors.New("module factory is nil")
	}
	started := make([]runModule, 0, len(names))
	for _, name := range names {
		mod, err := factory(name)
		if err != nil {
			return nil, combineRunErrors(fmt.Errorf("create module %q: %w", name, err), closeModules(started))
		}
		if mod == nil {
			return nil, combineRunErrors(fmt.Errorf("create module %q: nil module", name), closeModules(started))
		}
		if err := mod.Init(ctx, logger, conf); err != nil {
			return nil, combineRunErrors(fmt.Errorf("initialize module %q: %w", name, err), closeModules(started))
		}
		started = append(started, mod)
		if err := mod.Run(); err != nil {
			return nil, combineRunErrors(fmt.Errorf("run module %q: %w", name, err), closeModules(started))
		}
	}
	return started, nil
}

func closeModules(modules []runModule) error {
	var causes []error
	for index := len(modules) - 1; index >= 0; index-- {
		if modules[index] == nil {
			continue
		}
		if err := modules[index].Close(); err != nil {
			causes = append(causes, fmt.Errorf("close module %q: %w", modules[index].Name(), err))
		}
	}
	if len(causes) == 0 {
		return nil
	}
	return runErrors{prefix: "module cleanup failed", causes: causes}
}

func combineRunErrors(primary, cleanup error) error {
	if primary == nil {
		return cleanup
	}
	if cleanup == nil {
		return primary
	}
	return runErrors{prefix: "module operation failed", causes: []error{primary, cleanup}}
}

type runErrors struct {
	prefix string
	causes []error
}

func (err runErrors) Error() string {
	parts := make([]string, 0, len(err.causes))
	for _, cause := range err.causes {
		parts = append(parts, cause.Error())
	}
	return err.prefix + ": " + strings.Join(parts, "; ")
}

func (err runErrors) Is(target error) bool {
	for _, cause := range err.causes {
		if errors.Is(cause, target) {
			return true
		}
	}
	return false
}
