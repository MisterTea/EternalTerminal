package archive

// #cgo LDFLAGS: -lParallelCompression
// #include <stdio.h>
// #include <stdint.h>
// #include <stdlib.h>
//
// typedef struct
// {
//   int64_t unknown1;
//   int64_t unknown2;
//   char *input;
//   char *output;
//   char *patch;
//   uint32_t not_cryptex_cache;
//   uint32_t threads;
//   uint32_t verbose;
// } RawImage;
//
// extern int32_t RawImagePatch(RawImage *) __attribute__((weak));
import "C"

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"unsafe"
)

type ArchiveFormat int

const (
	IPSW ArchiveFormat = iota
	Installer
)

// ExtractCaches extracts any dyld shared caches from `archive` to `destination`.
func ExtractCaches(format ArchiveFormat, archive string, destination string, verbose bool) error {
	opts := ExtractorOptions{Verbose: true, Source: archive, Destination: destination}
	var e *Extractor
	switch format {
	case IPSW:
		e = NewIPSWExtractor(opts)
	case Installer:
		e = NewInstallAssistantExtractor(opts)
	default:
		return fmt.Errorf("unknown format %v", format)
	}
	return e.Extract()
}

// NewIPSWExtractor returns an `Extractor` that can handle `.ipsw` files.
func NewIPSWExtractor(opts ExtractorOptions) *Extractor {
	ie := &ipswExtractor{}
	ie.Extractor = &Extractor{opts: opts, impl: ie}
	return ie.Extractor
}

// NewInstallAssistantExtractor returns an extractor that can handle Apple installers.
func NewInstallAssistantExtractor(opts ExtractorOptions) *Extractor {
	ie := &installAssistantExtractor{}
	ie.Extractor = &Extractor{opts: opts, impl: ie}
	return ie.Extractor
}

// ExtractorOptions are provided to an extractor to specify source file, destination,
// and whether verbose logging should be used.
type ExtractorOptions struct {
	Verbose     bool
	Source      string
	Destination string
}

// Extractor encapsulates the process of extracting dyld shared caches from an IPSW or installer.
type Extractor struct {
	opts ExtractorOptions
	impl extractorImpl

	scratchDir string

	// If non-empty, the path at which the DMG was mounted. This will
	// be un-mounted at the end of Extract().
	dmgMountPaths []string
}

// Extract extracts any dyld shared caches in `opts.Source` to `opts.Destination`.
func (e *Extractor) Extract() error {
	scratchDir, err := os.MkdirTemp("", "extracted_system")
	if err != nil {
		return fmt.Errorf("couldn't create scratch directory to extract: %v", err)
	}
	defer os.RemoveAll(scratchDir)
	e.scratchDir = scratchDir

	err = e.impl.doExtract()

	for _, path := range e.dmgMountPaths {
		unmountErr := unmountDMG(path)
		if unmountErr != nil {
			err = errors.Join(err, unmountErr)
		}
	}

	return err
}

// vlog logs if `opts.Verbose` is set and is a no-op otherwise.
func (e *Extractor) vlog(format string, args ...interface{}) {
	if e.opts.Verbose {
		fmt.Printf(format+"\n", args...)
	}
}

// mountDMG mounts the disk image at `dmgPath` to mountpoint and tracks it so that
// it can be unmounted by the end of `Extract`
func (e *Extractor) mountDMG(dmgPath string, mountpoint string) error {
	cmd := exec.Command("hdiutil", "attach", dmgPath, "-mountpoint", mountpoint, "-quiet", "-nobrowse", "-readonly")
	err := cmd.Run()
	if err == nil {
		e.dmgMountPaths = append(e.dmgMountPaths, mountpoint)
	}
	return err
}

// extractorImpl is a private interface implemented by the backend
// extractors.
type extractorImpl interface {
	doExtract() error
}

// ipswExtractor extracts IPSWs
type ipswExtractor struct {
	*Extractor
}

// doExtract extracts dyld shared caches from an IPSW.
// It:
// Extracts the system disk image from the IPSW and mounts it.
// Copies any dyld shared caches from /System/Library/dyld on the mounted
// image to `opts.Destination`.
func (e *ipswExtractor) doExtract() error {
	e.vlog("Extracting and mounting system disk:\n")
	system, err := e.mountSystemDMG(e.opts.Source)
	if err != nil {
		return fmt.Errorf("couldn't mount system DMG: %v", err)
	}
	e.vlog("System mounted at %v\n", system)
	e.vlog("Extracting shared caches:\n")
	cachesPath := path.Join(system, "System/Library/dyld")
	if !pathExists(cachesPath) {
		return errors.New("couldn't find /System/Library/dyld")
	}

	caches, err := filepath.Glob(path.Join(cachesPath, "dyld_shared_cache*"))
	if err != nil {
		// "The only possible returned error is ErrBadPattern" so treat
		// this like a programmer error.
		log.Fatalf("Failed to glob %v", path.Join(cachesPath, "dyld_shared_cache*"))
	}

	for _, cache := range caches {
		src, err := os.Open(cache)
		if err != nil {
			return err
		}
		defer src.Close()
		filename := path.Base(cache)
		dst, err := os.Create(path.Join(e.opts.Destination, filename))
		if err != nil {
			return err
		}
		defer dst.Close()
		e.vlog("Extracted %v\n", filename)
		if _, err := io.Copy(dst, src); err != nil {
			return err
		}
	}

	return nil
}

// mountSystemDMG finds the name of the system image disk in the build manifest inside the
// IPSW at `ipswPath`, mounts it inside `e.scratchDir` and returns the mountpoint.
func (e *ipswExtractor) mountSystemDMG(ipswPath string) (string, error) {
	r, err := zip.OpenReader(ipswPath)
	if err != nil {
		return "", fmt.Errorf("couldn't open ipsw at %s: %v", ipswPath, err)
	}
	defer r.Close()
	dmgPath := ""
	for _, f := range r.File {
		if f.Name == "BuildManifest.plist" {
			manifest, err := os.Create(path.Join(e.scratchDir, f.Name))
			if err != nil {
				return "", err
			}
			if err := extractFileToPath(f, path.Join(e.scratchDir, f.Name)); err != nil {
				return "", err
			}
			path, err := e.getSystemDMGPath(manifest.Name())
			if err != nil {
				return "", err
			}
			dmgPath = path
		}
	}
	if dmgPath == "" {
		return "", errors.New("couldn't find build manifest")
	}
	for _, f := range r.File {
		if filepath.Base(f.Name) == dmgPath {
			dmgPath := path.Join(e.scratchDir, f.Name)
			if err := extractFileToPath(f, dmgPath); err != nil {
				return "", err
			}
			dmgMountpoint := path.Join(e.scratchDir, "Root")
			e.vlog("Mounting %v at %v\n", dmgPath, dmgMountpoint)
			if err := e.mountDMG(dmgPath, dmgMountpoint); err != nil {
				return "", err
			}
			return dmgMountpoint, nil
		}
	}
	return "", fmt.Errorf("%v not present in %v", dmgPath, ipswPath)
}

// getSystemDMGPath finds the system disk image inside a IPSW from the build manifest
// at `manifest`.
func (e *ipswExtractor) getSystemDMGPath(manifest string) (string, error) {
	print_cmd := "print :BuildIdentities:1:Manifest:Cryptex1,SystemOS:Info:Path"
	result, err := exec.Command("/usr/libexec/PlistBuddy", "-c", print_cmd, manifest).Output()
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(result)), nil
}

// installAssistantExtractor extracts Apple installers.
type installAssistantExtractor struct {
	*Extractor
}

// doExtract extracts dyld shared caches from an Apple installer.
// It:
//  1. Expands the installer to disk
//  2. Finds and mounts SharedSupport.dmg
//  3. Determines based on system version whether this installer contains cryptexes or not.
//  4. If cryptexes are present, extracts them to disk images, mounts them, then copies any shared caches
//     to `opts.Destination`. Otherwise, finds any payload zips containing shared caches, extracts them, and
//     copies the caches to `opts.Destination`
func (e *installAssistantExtractor) doExtract() error {
	expandedPath := path.Join(e.scratchDir, "installer")
	e.vlog("Expanding installer to %v\n", expandedPath)
	if err := e.expandInstaller(e.opts.Source, expandedPath); err != nil {
		return fmt.Errorf("expand installer: %v", err)
	}

	dmgPath := path.Join(expandedPath, "SharedSupport.dmg")
	if !pathExists(dmgPath) {
		return fmt.Errorf("couldn't find SharedSupport.dmg at %v", dmgPath)
	}
	dmgMountpoint := path.Join(e.scratchDir, "shared_support")
	e.vlog("Mounting %v at %v\n", dmgPath, dmgMountpoint)
	if err := e.mountDMG(dmgPath, dmgMountpoint); err != nil {
		return fmt.Errorf("mount %v: %v", dmgPath, err)
	}

	zipsPath := path.Join(dmgMountpoint, "com_apple_MobileAsset_MacSoftwareUpdate")
	if !pathExists(zipsPath) {
		return fmt.Errorf("couldn't find com_apple_MobileAsset_MacSoftwareUpdate on SharedSupport.dmg")
	}
	hasCryptexes, err := e.hasCryptexes(path.Join(zipsPath, "com_apple_MobileAsset_MacSoftwareUpdate.xml"))
	if err != nil {
		return fmt.Errorf("couldn't determine system version: %v", err)
	}
	zips, err := filepath.Glob(path.Join(zipsPath, "*.zip"))
	if err != nil {
		// "The only possible returned error is ErrBadPattern" so treat
		// this like a programmer error.
		log.Fatalf("Failed to glob %v", path.Join(zipsPath, "*.zip"))
	}
	return e.extractCachesFromZips(zips, e.opts.Destination, hasCryptexes)
}

// expandInstaller expands the installer at `installerPath` to `destinaton`.
func (e *installAssistantExtractor) expandInstaller(installerPath string, destination string) error {
	return exec.Command("pkgutil", "--expand-full", installerPath, destination).Run()
}

// hasCryptexes returns true if the installer containing the plist at `plistPath` is for
// macOS version 13 or higher, and accordingly stores dyld shared caches inside cryptexes.
func (e *installAssistantExtractor) hasCryptexes(plistPath string) (bool, error) {
	print_cmd := "print :Assets:0:OSVersion"
	result, err := exec.Command("/usr/libexec/PlistBuddy", "-c", print_cmd, plistPath).Output()
	if err != nil {
		return false, fmt.Errorf("couldn't read OS version from %s: %v", plistPath, err)
	}
	majorVersion := strings.Split(string(result), ".")[0]
	if v, err := strconv.Atoi(majorVersion); err != nil {
		return false, fmt.Errorf("couldn't parse major version %s:%v", majorVersion, err)
	} else {
		return v >= 13, nil
	}
}

// extractCachesFromZips extracts zips that contain dyld shared caches, and extracts the dyld shared caches from them.
// The specifics depend on whether this installer uses cryptexes or payload files.
func (e *installAssistantExtractor) extractCachesFromZips(zips []string, destination string, hasCryptexes bool) error {
	containerPath := path.Join(e.scratchDir, "container")
	if !hasCryptexes {
		if err := e.unarchiveZipsMatching(zips, containerPath, "AssetData/payloadv2/payload.0??"); err != nil {
			return err
		}
		if err := e.extractCachesFromPayloads(containerPath, destination); err != nil {
			return fmt.Errorf("couldn't extract caches from %v: %v", containerPath, err)
		}
	} else {
		if err := e.unarchiveZipsMatching(zips, containerPath, "AssetData/payloadv2/image_patches/cryptex-system-*"); err != nil {
			return err
		}
		if err := e.extractCachesFromCryptexes(containerPath, destination); err != nil {
			return fmt.Errorf("couldn't extract caches from %v: %v", containerPath, err)
		}
	}
	return nil
}

// unarchiveZipsMatching unarchives all files matching `glob` from the zip files in `zips` to destination.
func (e *installAssistantExtractor) unarchiveZipsMatching(zips []string, destination string, glob string) error {
	for _, zipFile := range zips {
		archive, err := zip.OpenReader(zipFile)
		if err != nil {
			return fmt.Errorf("couldn't read %v: %v", zipFile, err)
		}
		defer archive.Close()
		e.vlog("Unarchiving %v\n", zipFile)
		if err := e.unarchiveFilesMatching(archive, destination, glob); err != nil {
			return fmt.Errorf("couldn't unarchive files matching %v from %v", glob, zipFile)
		}
	}
	return nil
}

// unarchiveFilesMatching unarchives all files matching `glob` from `r` to destination.
func (e *installAssistantExtractor) unarchiveFilesMatching(r *zip.ReadCloser, destination string, glob string) error {
	if err := os.MkdirAll(destination, 0755); err != nil {
		return err
	}
	for _, file := range r.File {
		if file.FileInfo().IsDir() {
			continue
		}
		if ok, err := path.Match(glob, file.Name); !ok || err != nil {
			continue
		}
		_, filename := path.Split(file.Name)
		writePath := path.Join(destination, filename)
		if err := extractFileToPath(file, writePath); err != nil {
			return err
		}
	}
	return nil
}

// extractCachesFromPayloads unarchives any files containing dyld shared caches from `payloadsPath`
// then copies any shared caches to `destination`.
func (e *installAssistantExtractor) extractCachesFromPayloads(payloadsPath string, destination string) error {
	scratchDir, err := os.MkdirTemp(e.scratchDir, "payload")
	if err != nil {
		return err
	}
	files, err := os.ReadDir(payloadsPath)
	if err != nil {
		return err
	}
	for _, f := range files {
		payload := path.Join(payloadsPath, f.Name())
		if e.payloadHasSharedCache(payload) {
			e.vlog("Extracting %v\n", payload)
			if err := e.extractPayload(payload, scratchDir); err != nil {
				return err
			}
		}
	}
	return e.copySharedCaches(scratchDir, destination)
}

// payloadHasSharedCache returns true if the archive at `payloadPath` contains a dyld shared cache.
func (e *installAssistantExtractor) payloadHasSharedCache(payloadPath string) bool {
	out, err := exec.Command("yaa", "list", "-i", payloadPath).Output()
	return err == nil && bytes.Contains(out, []byte("/dyld_shared_cache"))
}

// extractPayload extracts the apple archive at `payloadPath` to `destination`.
func (e *installAssistantExtractor) extractPayload(payloadPath string, destination string) error {
	return exec.Command("yaa", "extract", "-i", payloadPath, "-d", destination).Run()
}

// copySharedCaches copies the contents of `System/Library/dyld` in `from` to `to`.
func (e *installAssistantExtractor) copySharedCaches(from, to string) error {
	dyldPath := path.Join(from, "System/Library/dyld")
	if !pathExists(dyldPath) {
		return fmt.Errorf("couldn't find System/Library/dyld in %s", dyldPath)
	}
	cacheFiles, err := os.ReadDir(dyldPath)
	if err != nil {
		return err
	}
	for _, cacheFile := range cacheFiles {
		name := cacheFile.Name()
		src := path.Join(dyldPath, name)
		dst := path.Join(to, name)
		e.vlog("Copying %v to %v\n", src, dst)
		if err := copyFile(src, dst); err != nil {
			return fmt.Errorf("couldn't copy %s to %s: %v", src, dst, err)
		}
	}
	return nil
}

// extractCachesFromCryptexes extracts disk images from any cryptexes at `cryptexesPath`, mounts them,
// and extracts any dyld shared caches to `destination`.
func (e *installAssistantExtractor) extractCachesFromCryptexes(cryptexesPath string, destination string) error {
	scratchDir, err := os.MkdirTemp(e.scratchDir, "cryptex_dmg")
	if err != nil {
		return err
	}
	files, err := os.ReadDir(cryptexesPath)
	if err != nil {
		return err
	}
	for _, f := range files {
		cryptexMountpoint := path.Join(scratchDir, "cryptex_"+f.Name())
		cryptex := path.Join(cryptexesPath, f.Name())
		dmgPath := path.Join(scratchDir, f.Name()+".dmg")
		e.extractCryptexDMG(cryptex, dmgPath)
		e.vlog("Mounting %s at %s\n", cryptex, dmgPath)
		e.mountDMG(dmgPath, cryptexMountpoint)
		if err := e.copySharedCaches(cryptexMountpoint, destination); err != nil {
			return err
		}
	}
	return nil
}

// extractCryptexDMG extracts the cryptex at `cryptexPath` to `dmgPath` using libParallelCompression.
func (e *installAssistantExtractor) extractCryptexDMG(cryptexPath, dmgPath string) error {
	inp := C.CString("")
	defer C.free(unsafe.Pointer(inp))
	cryptex := C.CString(cryptexPath)
	defer C.free(unsafe.Pointer(cryptex))
	result := C.CString(dmgPath)
	defer C.free(unsafe.Pointer(result))

	ri := C.RawImage{unknown1: 0, unknown2: 0, input: inp, output: result, patch: cryptex, not_cryptex_cache: 0, threads: 0, verbose: 1}
	if exitCode := C.RawImagePatch(&ri); exitCode != 0 {
		return fmt.Errorf("RawImagePatch failed with %d", exitCode)
	}
	return nil
}

// pathExists returns true if `path` exists.
func pathExists(path string) bool {
	_, err := os.Stat(path)
	return !os.IsNotExist(err)
}

// extractFileToPath extracts the contents of `f` to `path`.
func extractFileToPath(f *zip.File, path string) error {
	w, err := os.Create(path)
	if err != nil {
		return err
	}
	defer w.Close()
	reader, err := f.Open()
	if err != nil {
		return err
	}

	if _, err := io.Copy(w, reader); err != nil {
		return err
	}
	return nil
}

// copyFile copies `src` to `dst`, which can be on different volumes.
func copyFile(src, dst string) error {
	w, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer w.Close()

	reader, err := os.Open(src)
	if err != nil {
		return err
	}
	defer reader.Close()
	if _, err := io.Copy(w, reader); err != nil {
		return err
	}
	return nil
}

// unmountDMG unmounts the disk image at `mountpoint`.
func unmountDMG(mountpoint string) error {
	return exec.Command("hdiutil", "detach", mountpoint).Run()
}
