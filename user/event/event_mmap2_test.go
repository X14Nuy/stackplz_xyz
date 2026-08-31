package event

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadMapsFileFindsLibraryByNameAndPath(t *testing.T) {
	const pid = 71001
	const fullPath = "/data/app/example/lib/arm64/libtarget.so"
	mapsFile := filepath.Join(t.TempDir(), "maps.txt")
	content := "71000000-71001000 r--p 00000000 00:00 0 " + fullPath + "\n" +
		"71001000-71002000 r-xp 00001000 00:00 0 " + fullPath + "\n"
	if err := os.WriteFile(mapsFile, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	if err := LoadMapsFile(pid, mapsFile); err != nil {
		t.Fatal(err)
	}
	for _, query := range []string{"libtarget.so", fullPath} {
		info, err := FindLibInMaps(pid, query)
		if err != nil {
			t.Fatalf("FindLibInMaps(%q): %v", query, err)
		}
		if info.BaseAddr != 0x71000000 || info.LibPath != fullPath {
			t.Fatalf("FindLibInMaps(%q)=%#v", query, info)
		}
	}
}

func TestLoadMapsFileReplacesCachedMaps(t *testing.T) {
	const pid = 71002
	dir := t.TempDir()
	first := filepath.Join(dir, "first.maps")
	second := filepath.Join(dir, "second.maps")
	if err := os.WriteFile(first, []byte("72000000-72001000 r-xp 0 00:00 0 /old.so\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(second, []byte("73000000-73001000 r-xp 0 00:00 0 /new.so\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := LoadMapsFile(pid, first); err != nil {
		t.Fatal(err)
	}
	if err := LoadMapsFile(pid, second); err != nil {
		t.Fatal(err)
	}
	if _, err := FindLibInMaps(pid, "old.so"); err == nil {
		t.Fatal("stale map survived replacement")
	}
	if info, err := FindLibInMaps(pid, "new.so"); err != nil || info.BaseAddr != 0x73000000 {
		t.Fatalf("new map=(%#v,%v)", info, err)
	}
}

func TestLoadMapsFileReportsReadAndLookupErrors(t *testing.T) {
	const pid = 71003
	missing := filepath.Join(t.TempDir(), "missing.maps")
	if err := LoadMapsFile(pid, missing); err == nil {
		t.Fatal("missing maps file accepted")
	}
	mapsFile := filepath.Join(t.TempDir(), "maps.txt")
	if err := os.WriteFile(mapsFile, []byte("74000000-74001000 r-xp 0 00:00 0 /present.so\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := LoadMapsFile(pid, mapsFile); err != nil {
		t.Fatal(err)
	}
	if _, err := FindLibInMaps(pid, "missing.so"); err == nil {
		t.Fatal("missing library accepted")
	}
}

func TestFindLibInMapsRejectsAmbiguousBasename(t *testing.T) {
	const pid = 71004
	dir := t.TempDir()
	mapsFile := filepath.Join(dir, "maps.txt")
	content := "75000000-75001000 r-xp 0 00:00 0 /one/libsame.so\n" +
		"76000000-76001000 r-xp 0 00:00 0 /two/libsame.so\n"
	if err := os.WriteFile(mapsFile, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	if err := LoadMapsFile(pid, mapsFile); err != nil {
		t.Fatal(err)
	}
	if _, err := FindLibInMaps(pid, "libsame.so"); err == nil {
		t.Fatal("ambiguous basename accepted")
	}
	if info, err := FindLibInMaps(pid, "/two/libsame.so"); err != nil || info.BaseAddr != 0x76000000 {
		t.Fatalf("exact path=(%#v,%v)", info, err)
	}
}

func TestLoadMapsContentRejectsMalformedReplacementAtomically(t *testing.T) {
	const pid = 71005
	if err := LoadMapsContent(pid, []byte("77000000-77001000 r-xp 0 00:00 0 /kept.so\n")); err != nil {
		t.Fatal(err)
	}
	if err := LoadMapsContent(pid, []byte("not a proc maps line\n")); err == nil {
		t.Fatal("malformed replacement accepted")
	}
	info, err := FindLibInMaps(pid, "kept.so")
	if err != nil || info.BaseAddr != 0x77000000 {
		t.Fatalf("previous cache was not preserved: info=%#v err=%v", info, err)
	}
}
