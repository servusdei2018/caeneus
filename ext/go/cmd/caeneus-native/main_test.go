package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestChecksumFor(t *testing.T) {
	asset := "caeneus-v0.1.0-linux_amd64.tar.gz"
	sum := sha256.Sum256([]byte("archive"))
	body := hex.EncodeToString(sum[:]) + "  " + asset + "\n"

	got, err := checksumFor([]byte(body), asset)
	if err != nil {
		t.Fatal(err)
	}
	if got != hex.EncodeToString(sum[:]) {
		t.Fatalf("got %q", got)
	}
}

func TestExtractArchive(t *testing.T) {
	archive := makeArchive(t, map[string]string{
		"libcaeneus.a": "static",
		"caeneus.h":    "header",
	})
	dest := t.TempDir()

	if err := extractArchive(archive, dest, "linux_amd64"); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"libcaeneus.a", "caeneus.h"} {
		if _, err := os.Stat(filepath.Join(dest, name)); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
	}
}

func TestExtractArchiveRejectsExtra(t *testing.T) {
	archive := makeArchive(t, map[string]string{
		"libcaeneus.a": "static",
		"caeneus.h":    "header",
		"evil.so":      "nope",
	})
	if err := extractArchive(archive, t.TempDir(), "linux_amd64"); err == nil {
		t.Fatal("expected error")
	}
}

func TestExports(t *testing.T) {
	out := exports("/cache/linux_amd64", "linux_amd64")
	if !strings.Contains(out, "CGO_LDFLAGS") || !strings.Contains(out, "libcaeneus.a") {
		t.Fatalf("unexpected output: %s", out)
	}
}

func TestResolveDestDefault(t *testing.T) {
	got, err := resolveDest("", "v0.1.0")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(got, filepath.Join(".cache", "caeneus", "v0.1.0")) {
		t.Fatalf("got %q", got)
	}
}

func makeArchive(t *testing.T, files map[string]string) []byte {
	t.Helper()
	var buf bytes.Buffer
	gz := gzip.NewWriter(&buf)
	tw := tar.NewWriter(gz)
	for name, contents := range files {
		if err := tw.WriteHeader(&tar.Header{Name: name, Mode: 0644, Size: int64(len(contents))}); err != nil {
			t.Fatal(err)
		}
		if _, err := tw.Write([]byte(contents)); err != nil {
			t.Fatal(err)
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := gz.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}
