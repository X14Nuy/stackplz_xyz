package cmd

import (
	"context"
	"errors"
	"io"
	"log"
	"reflect"
	"testing"

	"stackplz/user/config"
)

type fakeRunModule struct {
	name       string
	initErr    error
	runErr     error
	closeErr   error
	order      *[]string
	initCalls  int
	runCalls   int
	closeCalls int
}

func (mod *fakeRunModule) Name() string { return mod.name }

func (mod *fakeRunModule) Init(context.Context, *log.Logger, config.IConfig) error {
	mod.initCalls++
	*mod.order = append(*mod.order, "init:"+mod.name)
	return mod.initErr
}

func (mod *fakeRunModule) Run() error {
	mod.runCalls++
	*mod.order = append(*mod.order, "run:"+mod.name)
	return mod.runErr
}

func (mod *fakeRunModule) Close() error {
	mod.closeCalls++
	*mod.order = append(*mod.order, "close:"+mod.name)
	return mod.closeErr
}

func TestStartModulesRollsBackInReverseOrder(t *testing.T) {
	runErr := errors.New("second run failed")
	closeErr := errors.New("first close failed")
	var order []string
	mods := map[string]*fakeRunModule{
		"first":  {name: "first", closeErr: closeErr, order: &order},
		"second": {name: "second", runErr: runErr, order: &order},
	}
	_, err := startModules(context.Background(), log.New(io.Discard, "", 0), config.NewModuleConfig(), []string{"first", "second"}, func(name string) (runModule, error) {
		return mods[name], nil
	})
	if !errors.Is(err, runErr) || !errors.Is(err, closeErr) {
		t.Fatalf("error=%v", err)
	}
	want := []string{"init:first", "run:first", "init:second", "run:second", "close:second", "close:first"}
	if !reflect.DeepEqual(order, want) {
		t.Fatalf("order=%v want=%v", order, want)
	}
}

func TestStartModulesDoesNotCloseModuleWhoseInitFailed(t *testing.T) {
	initErr := errors.New("second init failed")
	var order []string
	mods := map[string]*fakeRunModule{
		"first":  {name: "first", order: &order},
		"second": {name: "second", initErr: initErr, order: &order},
	}
	_, err := startModules(context.Background(), log.New(io.Discard, "", 0), config.NewModuleConfig(), []string{"first", "second"}, func(name string) (runModule, error) {
		return mods[name], nil
	})
	if !errors.Is(err, initErr) {
		t.Fatalf("error=%v", err)
	}
	want := []string{"init:first", "run:first", "init:second", "close:first"}
	if !reflect.DeepEqual(order, want) {
		t.Fatalf("order=%v want=%v", order, want)
	}
	if mods["second"].closeCalls != 0 {
		t.Fatalf("module whose Init failed was closed %d times", mods["second"].closeCalls)
	}
}

func TestCloseModulesAttemptsEveryModuleOnce(t *testing.T) {
	firstErr := errors.New("first close failed")
	secondErr := errors.New("second close failed")
	var order []string
	mods := []runModule{
		&fakeRunModule{name: "first", closeErr: firstErr, order: &order},
		&fakeRunModule{name: "second", closeErr: secondErr, order: &order},
	}
	err := closeModules(mods)
	if !errors.Is(err, firstErr) || !errors.Is(err, secondErr) {
		t.Fatalf("error=%v", err)
	}
	if !reflect.DeepEqual(order, []string{"close:second", "close:first"}) {
		t.Fatalf("order=%v", order)
	}
	for _, mod := range mods {
		if mod.(*fakeRunModule).closeCalls != 1 {
			t.Fatalf("module %#v closed wrong number of times", mod)
		}
	}
}
