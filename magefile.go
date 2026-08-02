//go:build mage

package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// Default is the target run when `mage` is invoked with no arguments.
var Default = Build

// VS-bundled toolchain. Machine-specific to the user's BuildTools install, but
// capturing it here is the whole point: it avoids depending on PATH state.
const (
	cmakeExe = `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
)

// clang-format is pinned to the exact version CI installs, and is NOT taken from the
// Visual Studio toolchain alongside cmakeExe above. VS bundles 19.1.5 while CI resolves
// obsproject/tools/clang-format@19 to 19.1.1, and patch releases of clang-format disagree
// about output -- so the bundled binary reformats files in ways CI then rejects, producing
// a local verdict that cannot be acted on. That mismatch is how ten files reached master
// unformatted while a local `mage format` reported success.
const (
	clangFormatVersion = "19.1.1"
	// PyPI file URLs are immutable, so the URL and the hash pin together. On a version
	// bump take both from https://pypi.org/pypi/clang-format/<version>/json, the
	// win_amd64 entry under "urls".
	clangFormatURL     = "https://files.pythonhosted.org/packages/89/6e/ce69b88264d8e00712262401e2d72957f20bb609c523cd7bdfc3b96fc83a/clang_format-19.1.1-py2.py3-none-win_amd64.whl"
	clangFormatSHA256  = "631aa55a56f96f12d9c6060fb6d09f397f32705c7a4331e1944a107f6c99f139"
	clangFormatInWheel = "clang_format/data/bin/clang-format.exe"
)

const (
	buildDir       = "build_x64"
	buildConfig    = "RelWithDebInfo"
	configurePset  = "windows-x64"
	frontendTarget = "braidcast-frontend"
	versionTag     = "32.1.0"
	depsDir        = ".deps"
)

// runExe is the full runtime layout produced by the build; running anything
// else won't resolve OBS's data/plugins correctly.
var runExe = filepath.Join(buildDir, "rundir", buildConfig, "bin", "64bit", "braidcast.exe")

// Build does an incremental frontend build (the braidcast-frontend target only).
func Build() error {
	return sh(cmakeExe, "--build", buildDir, "--config", buildConfig, "--target", frontendTarget)
}

// BuildAll does a full build of every target.
func BuildAll() error {
	return sh(cmakeExe, "--build", buildDir, "--config", buildConfig)
}

// Configure runs the CMake configure preset. It first ensures a valid version
// tag and the prefetched dependency zips exist, because the configure step
// derives the version from git and downloads deps during configuration.
func Configure() error {
	if err := Tag(); err != nil {
		return err
	}
	if err := Deps(); err != nil {
		return err
	}
	return sh(cmakeExe, "--preset", configurePset)
}

// Tag forces a 32.1.0 git tag so project(VERSION) derivation works. Without a
// tag `git describe` returns a bare hash and the CMake version parse fails.
// Idempotent: -f overwrites if it already points elsewhere.
func Tag() error {
	return sh("git", "tag", "-f", versionTag)
}

// Run launches the built OBS in portable mode. --portable is mandatory: it
// keeps OBS pointed at the rundir instead of %APPDATA%\obs-studio, protecting
// the user's real profiles. The working directory is set to the exe's folder
// so OBS resolves its relative data/plugin paths.
func Run() error {
	if _, err := os.Stat(runExe); err != nil {
		if os.IsNotExist(err) {
			return fmt.Errorf("%s not found — run `mage build` first", runExe)
		}
		return err
	}
	// Resolve to an absolute path: with cmd.Dir set, Windows fails to find a
	// relative executable (it gets re-resolved against cmd.Dir).
	exe, err := filepath.Abs(runExe)
	if err != nil {
		return err
	}
	cmd := exec.Command(exe, "--portable")
	cmd.Dir = filepath.Dir(exe)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	fmt.Printf("> %s --portable\n", exe)
	return cmd.Run()
}

var cppExts = map[string]bool{
	".c": true, ".cc": true, ".cpp": true, ".cxx": true,
	".h": true, ".hpp": true, ".hxx": true,
	".m": true, ".mm": true,
}

var formatSkipDirs = []string{buildDir, "deps", depsDir, "frontend/deps"}

// Format runs clang-format in place over the C/C++ files changed vs HEAD
// (staged, unstaged, and untracked), using the repo's .clang-format style.
func Format() error {
	files, err := cppFilesFrom([][]string{
		{"diff", "--name-only", "HEAD"},
		{"diff", "--name-only", "--cached"},
		{"ls-files", "--others", "--exclude-standard"},
	})
	if err != nil {
		return err
	}
	if len(files) == 0 {
		fmt.Println("no changed C/C++ files")
		return nil
	}
	exe, err := ensureClangFormat()
	if err != nil {
		return err
	}

	fmt.Printf("formatting %d file(s):\n", len(files))
	for _, f := range files {
		fmt.Printf("  %s\n", f)
	}

	return sh(exe, append([]string{"-i", "--style=file"}, files...)...)
}

// FormatCheck reports whether anything on this branch would fail CI's format job,
// changing nothing on disk.
//
// It compares against master rather than HEAD, which is the one thing Format cannot do:
// the drift that actually breaks CI lives in files that are already committed, so a
// changed-vs-HEAD view sees a clean tree and says nothing is wrong. This mirrors what the
// format job examines once work reaches master, which is the moment the whole accumulated
// batch is checked at once.
func FormatCheck() error {
	files, err := cppFilesFrom([][]string{{"diff", "--name-only", "master...HEAD"}})
	if err != nil {
		return err
	}
	if len(files) == 0 {
		fmt.Println("no C/C++ files differ from master")
		return nil
	}
	exe, err := ensureClangFormat()
	if err != nil {
		return err
	}

	fmt.Printf("checking %d file(s) against clang-format %s\n", len(files), clangFormatVersion)
	return sh(exe, append([]string{"--dry-run", "-Werror", "--style=file"}, files...)...)
}

// cppFilesFrom collects the C/C++ files named by each git invocation, deduplicated across
// them, skipping vendored trees and anything that is no longer a file on disk (a deletion
// still shows up in `git diff --name-only`).
func cppFilesFrom(gitArgs [][]string) ([]string, error) {
	seen := make(map[string]bool)
	var files []string
	for _, args := range gitArgs {
		out, err := shCapture("git", args...)
		if err != nil {
			return nil, err
		}
		for _, l := range strings.Split(out, "\n") {
			l = strings.TrimSpace(l)
			if l == "" || seen[l] {
				continue
			}
			seen[l] = true
			if !cppExts[strings.ToLower(filepath.Ext(l))] {
				continue
			}
			if inSkippedDir(l) {
				continue
			}
			if info, err := os.Stat(l); err != nil || info.IsDir() {
				continue
			}
			files = append(files, l)
		}
	}
	return files, nil
}

// ensureClangFormat unpacks the pinned clang-format into .deps/ and returns its path,
// downloading it only on the first call. Same shape as ensureDep below, and --ssl-no-revoke
// is needed here for the same reason: the local TLS proxy breaks OCSP revocation lookups.
// The certificate chain is still verified, and the sha256 is what authenticates the payload.
func ensureClangFormat() (string, error) {
	dir := filepath.Join(depsDir, "clang-format-"+clangFormatVersion)
	exe := filepath.Join(dir, "clang-format.exe")
	if _, err := os.Stat(exe); err == nil {
		return exe, nil
	} else if !os.IsNotExist(err) {
		return "", err
	}

	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	wheel := filepath.Join(dir, "clang-format.whl")
	fmt.Printf("download  clang-format %s\n", clangFormatVersion)
	if err := sh("curl.exe", "-L", "--ssl-no-revoke", "-o", wheel, clangFormatURL); err != nil {
		return "", err
	}

	got, err := sha256File(wheel)
	if err != nil {
		return "", err
	}
	if !strings.EqualFold(got, clangFormatSHA256) {
		os.Remove(wheel)
		return "", fmt.Errorf("sha256 mismatch for clang-format %s: got %s, want %s",
			clangFormatVersion, got, clangFormatSHA256)
	}
	if err := extractFromZip(wheel, clangFormatInWheel, exe); err != nil {
		return "", err
	}
	os.Remove(wheel)
	fmt.Printf("verified  clang-format %s\n", clangFormatVersion)
	return exe, nil
}

// extractFromZip writes one named member of a zip archive to dest. A wheel is a zip, so
// this is the whole of what unpacking one needs.
func extractFromZip(archivePath, member, dest string) error {
	zr, err := zip.OpenReader(archivePath)
	if err != nil {
		return err
	}
	defer zr.Close()

	for _, f := range zr.File {
		if f.Name != member {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		defer rc.Close()
		out, err := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o755)
		if err != nil {
			return err
		}
		defer out.Close()
		_, err = io.Copy(out, rc)
		return err
	}
	return fmt.Errorf("%s not found in %s", member, archivePath)
}

func inSkippedDir(p string) bool {
	clean := filepath.ToSlash(filepath.Clean(p))
	for _, dir := range formatSkipDirs {
		prefix := filepath.ToSlash(filepath.Clean(dir)) + "/"
		if strings.HasPrefix(clean, prefix) {
			return true
		}
	}
	return false
}

// --- dependency prefetch ---------------------------------------------------

// depEntry describes one zip to ensure-present in .deps/.
type depEntry struct {
	url    string
	sha256 string
}

// presetsFile mirrors only the fields of CMakePresets.json we need.
type presetsFile struct {
	ConfigurePresets []struct {
		Name   string `json:"name"`
		Vendor struct {
			OBS struct {
				Dependencies depsBlock `json:"dependencies"`
			} `json:"obsproject.com/obs-studio"`
		} `json:"vendor"`
	} `json:"configurePresets"`
}

type depsBlock struct {
	Prebuilt depComponent `json:"prebuilt"`
	Qt6      depComponent `json:"qt6"`
	Cef      depComponent `json:"cef"`
}

type depComponent struct {
	Version  string            `json:"version"`
	BaseURL  string            `json:"baseUrl"`
	Hashes   map[string]string `json:"hashes"`
	Revision map[string]int    `json:"revision"`
}

// Deps prefetches the windows-x64 dependency zips into .deps/, reading their
// versions/hashes from CMakePresets.json so it survives dependency bumps.
// Norton 360 MITMs TLS and breaks OCSP revocation, so CMake's file(DOWNLOAD)
// dies with CRYPT_E_NO_REVOCATION_CHECK; we prefetch via curl.exe
// --ssl-no-revoke (still verifies the cert chain, only skips the broken
// revocation lookup) into the dir CMake's _check_dependencies probes.
func Deps() error {
	raw, err := os.ReadFile("CMakePresets.json")
	if err != nil {
		return err
	}
	var pf presetsFile
	if err := json.Unmarshal(raw, &pf); err != nil {
		return fmt.Errorf("parse CMakePresets.json: %w", err)
	}

	var d *depsBlock
	for i := range pf.ConfigurePresets {
		if pf.ConfigurePresets[i].Name == "dependencies" {
			d = &pf.ConfigurePresets[i].Vendor.OBS.Dependencies
			break
		}
	}
	if d == nil {
		return fmt.Errorf("could not find the \"dependencies\" configure preset in CMakePresets.json")
	}

	pb, qt, cef := d.Prebuilt, d.Qt6, d.Cef
	entries := []depEntry{
		{
			url:    fmt.Sprintf("%s/%s/windows-deps-%s-x64.zip", pb.BaseURL, pb.Version, pb.Version),
			sha256: pb.Hashes["windows-x64"],
		},
		{
			// x86 prebuilt is consumed by the nested x86 child CMake project.
			url:    fmt.Sprintf("%s/%s/windows-deps-%s-x86.zip", pb.BaseURL, pb.Version, pb.Version),
			sha256: pb.Hashes["windows-x86"],
		},
		{
			url:    fmt.Sprintf("%s/%s/windows-deps-qt6-%s-x64.zip", qt.BaseURL, qt.Version, qt.Version),
			sha256: qt.Hashes["windows-x64"],
		},
		{
			// CEF differs: baseUrl/file directly (no version path segment) with
			// a _v{revision} suffix on the filename.
			url:    fmt.Sprintf("%s/cef_binary_%s_windows_x64_v%d.zip", cef.BaseURL, cef.Version, cef.Revision["windows-x64"]),
			sha256: cef.Hashes["windows-x64"],
		},
	}

	if err := os.MkdirAll(depsDir, 0o755); err != nil {
		return err
	}

	for _, e := range entries {
		if err := ensureDep(e); err != nil {
			return err
		}
	}
	return nil
}

func ensureDep(e depEntry) error {
	name := e.url[strings.LastIndex(e.url, "/")+1:]
	dest := filepath.Join(depsDir, name)

	if _, err := os.Stat(dest); err == nil {
		got, err := sha256File(dest)
		if err != nil {
			return err
		}
		if strings.EqualFold(got, e.sha256) {
			fmt.Printf("cached    %s\n", name)
			return nil
		}
		fmt.Printf("mismatch  %s (have %s, want %s) — re-downloading\n", name, got, e.sha256)
		if err := os.Remove(dest); err != nil {
			return err
		}
	} else if !os.IsNotExist(err) {
		return err
	}

	fmt.Printf("download  %s\n", name)
	if err := sh("curl.exe", "-L", "--ssl-no-revoke", "-o", dest, e.url); err != nil {
		return err
	}

	got, err := sha256File(dest)
	if err != nil {
		return err
	}
	if !strings.EqualFold(got, e.sha256) {
		os.Remove(dest)
		return fmt.Errorf("sha256 mismatch for %s: got %s, want %s", name, got, e.sha256)
	}
	fmt.Printf("verified  %s\n", name)
	return nil
}

func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// --- shell helpers ---------------------------------------------------------

// sh runs a command, streaming stdout/stderr/stdin to the parent. The returned
// error already carries the real exit status because we exec directly — no
// pipe or shell wrapper masks the child's exit code.
func sh(name string, args ...string) error {
	fmt.Printf("> %s %s\n", name, strings.Join(args, " "))
	cmd := exec.Command(name, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	return cmd.Run()
}

// shCapture runs a command and returns its trimmed stdout. Stderr streams to
// the parent so failures stay visible.
func shCapture(name string, args ...string) (string, error) {
	cmd := exec.Command(name, args...)
	cmd.Stderr = os.Stderr
	out, err := cmd.Output()
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(out)), nil
}
