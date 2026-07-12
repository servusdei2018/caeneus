// Command caeneus-native stages the static C-ABI archive for Go/cgo.
//
// From a release tag:
//
//	eval "$(go run github.com/servusdei2018/caeneus/cmd/caeneus-native@v0.1.0 --version v0.1.0)"
//
// From a Caeneus checkout (requires Zig):
//
//	eval "$(go run ./ext/go/cmd/caeneus-native --local)"
package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const (
	githubRepo   = "servusdei2018/caeneus"
	localVersion = "local"
	fetchTimeout = 60 * time.Second
)

func main() {
	local := flag.Bool("local", false, "build with Zig from a Caeneus checkout")
	version := flag.String("version", "", "release tag to download, for example v0.1.0")
	dest := flag.String("dest", "", "staging root (default: user cache)")
	flag.Parse()

	if *local && *version != "" {
		fatalf("use either --local or --version, not both")
	}

	platform := runtime.GOOS + "_" + runtime.GOARCH
	ver := *version
	if *local {
		ver = localVersion
	} else if ver == "" || !strings.HasPrefix(ver, "v") {
		fatalf("--version is required (for example v0.1.0) unless --local is used")
	}

	root, err := resolveDest(*dest, ver)
	if err != nil {
		fatalf("%v", err)
	}
	platformDir := filepath.Join(root, platform)

	if *local {
		if err := stageLocal(platformDir); err != nil {
			fatalf("%v", err)
		}
	} else {
		if err := stageRelease(ver, platformDir, platform); err != nil {
			fatalf("%v", err)
		}
	}

	fmt.Print(exports(platformDir, platform))
}

func resolveDest(dest, version string) (string, error) {
	if dest != "" {
		return filepath.Clean(dest), nil
	}
	if version == localVersion {
		home, err := os.UserHomeDir()
		if err != nil {
			return "", err
		}
		return filepath.Join(home, ".cache", "caeneus", localVersion), nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".cache", "caeneus", version), nil
}

func exports(platformDir, platform string) string {
	archive := filepath.Join(platformDir, staticLib(platform))
	return fmt.Sprintf(
		"export CGO_LDFLAGS=%q${CGO_LDFLAGS:+ $CGO_LDFLAGS}\n",
		archive,
	)
}

func staticLib(platform string) string {
	if strings.HasPrefix(platform, "windows_") {
		return "caeneus-static.lib"
	}
	return "libcaeneus.a"
}

func stageLocal(platformDir string) error {
	tempDir, err := os.MkdirTemp("", "caeneus-native-build-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tempDir)

	repositoryRoot, err := findRepositoryRoot()
	if err != nil {
		return err
	}

	cmd := exec.Command("zig", "build", "-Doptimize=ReleaseFast", "-p", tempDir)
	cmd.Dir = repositoryRoot
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("zig build: %w", err)
	}

	libName := staticLib(runtime.GOOS + "_" + runtime.GOARCH)
	libPath := filepath.Join(tempDir, "lib", libName)
	if _, err := os.Stat(libPath); err != nil {
		return fmt.Errorf("missing %s after build: %w", libName, err)
	}

	return stageFiles(platformDir, map[string]string{
		libName:     libPath,
		"caeneus.h": filepath.Join(repositoryRoot, "include", "caeneus.h"),
	})
}

func findRepositoryRoot() (string, error) {
	current, err := os.Getwd()
	if err != nil {
		return "", err
	}

	for {
		if _, err := os.Stat(filepath.Join(current, "build.zig")); err == nil {
			if _, err := os.Stat(filepath.Join(current, "include", "caeneus.h")); err == nil {
				return current, nil
			}
		}
		parent := filepath.Dir(current)
		if parent == current {
			return "", errors.New("could not locate Caeneus checkout; run from ext/go or the repository root")
		}
		current = parent
	}
}

func stageRelease(version, platformDir, platform string) error {
	asset := fmt.Sprintf("caeneus-%s-%s.tar.gz", version, platform)
	url := fmt.Sprintf("https://github.com/%s/releases/download/%s/%s", githubRepo, version, asset)

	archive, err := fetch(url)
	if err != nil {
		return fmt.Errorf("download %s: %w", asset, err)
	}

	checksums, err := fetch(fmt.Sprintf("https://github.com/%s/releases/download/%s/checksums.txt", githubRepo, version))
	if err != nil {
		return fmt.Errorf("download checksums.txt: %w", err)
	}
	expected, err := checksumFor(checksums, asset)
	if err != nil {
		return err
	}
	actual := sha256.Sum256(archive)
	if hex.EncodeToString(actual[:]) != expected {
		return fmt.Errorf("checksum mismatch for %s", asset)
	}

	tempDir, err := os.MkdirTemp("", "caeneus-native-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tempDir)

	if err := extractArchive(archive, tempDir, platform); err != nil {
		return err
	}

	files := map[string]string{"caeneus.h": filepath.Join(tempDir, "caeneus.h")}
	files[staticLib(platform)] = filepath.Join(tempDir, staticLib(platform))
	return stageFiles(platformDir, files)
}

func fetch(url string) ([]byte, error) {
	client := &http.Client{Timeout: fetchTimeout}
	resp, err := client.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %s", resp.Status)
	}
	return io.ReadAll(io.LimitReader(resp.Body, 64<<20))
}

func checksumFor(contents []byte, filename string) (string, error) {
	for _, line := range strings.Split(string(contents), "\n") {
		fields := strings.Fields(line)
		if len(fields) == 2 && fields[1] == filename {
			return strings.ToLower(fields[0]), nil
		}
	}
	return "", fmt.Errorf("%s not found in checksums.txt", filename)
}

func extractArchive(contents []byte, dest, platform string) error {
	gz, err := gzip.NewReader(bytes.NewReader(contents))
	if err != nil {
		return err
	}
	defer gz.Close()

	want := map[string]bool{
		"caeneus.h":          true,
		staticLib(platform): true,
	}

	tr := tar.NewReader(gz)
	for {
		header, err := tr.Next()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return err
		}
		if header.Typeflag == tar.TypeDir {
			continue
		}
		name := filepath.Base(header.Name)
		if !want[name] {
			return fmt.Errorf("unexpected file in archive: %s", name)
		}
		data, err := io.ReadAll(io.LimitReader(tr, 64<<20))
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(dest, name), data, 0644); err != nil {
			return err
		}
		delete(want, name)
	}
	for name := range want {
		return fmt.Errorf("archive missing %s", name)
	}
	return nil
}

func stageFiles(platformDir string, files map[string]string) error {
	if err := os.RemoveAll(platformDir); err != nil {
		return err
	}
	if err := os.MkdirAll(platformDir, 0755); err != nil {
		return err
	}
	for name, src := range files {
		if err := copyFile(src, filepath.Join(platformDir, name)); err != nil {
			return fmt.Errorf("stage %s: %w", name, err)
		}
	}
	return nil
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "caeneus-native: "+format+"\n", args...)
	os.Exit(1)
}
