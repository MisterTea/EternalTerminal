/* Copyright 2014 Google LLC

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

 * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
 * Neither the name of Google LLC nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Tool upload_system_symbols generates and uploads Breakpad symbol files for OS X system libraries.

This tool shells out to the dump_syms and symupload Breakpad tools. In its default mode, this
will find all dynamic libraries on the system, run dump_syms to create the Breakpad symbol files,
and then upload them to Google's crash infrastructure.

The tool can also be used to only dump libraries or upload from a directory. See -help for more
information.

Both i386 and x86_64 architectures will be dumped and uploaded.
*/
package main

import (
	"debug/macho"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"upload_system_symbols/arch"
	"upload_system_symbols/archive"
)

var (
	breakpadTools    = flag.String("breakpad-tools", "out/Release/", "Path to the Breakpad tools directory, containing dump_syms and symupload.")
	uploadOnlyPath   = flag.String("upload-from", "", "Upload a directory of symbol files that has been dumped independently.")
	dumpOnlyPath     = flag.String("dump-to", "", "Dump the symbols to the specified directory, but do not upload them.")
	systemRoot       = flag.String("system-root", "", "Path to the root of the macOS system whose symbols will be dumped. Mutually exclusive with --installer and --ipsw.")
	dumpArchitecture = flag.String("arch", "", "The CPU architecture for which symbols should be dumped. If not specified, dumps all architectures.")
	apiKey           = flag.String("api-key", "", "API key to use. If this is present, the `sym-upload-v2` protocol is used.\nSee https://chromium.googlesource.com/breakpad/breakpad/+/HEAD/docs/sym_upload_v2_protocol.md or\n`symupload`'s help for more information.")
	installer        = flag.String("installer", "", "Path to macOS installer. Mutually exclusive with --system-root and --ipsw.")
	ipsw             = flag.String("ipsw", "", "Path to macOS IPSW. Mutually exclusive with --system-root and --installer.")
	separateArch     = flag.Bool("separate-arch", false, "Whether to separate symbols into architecture-specific directories when dumping.")
)

var (
	// pathsToScan are the subpaths in the systemRoot that should be scanned for shared libraries.
	pathsToScan = []string{
		"/System/Library/Frameworks",
		"/System/Library/PrivateFrameworks",
		"/usr/lib",
	}

	// optionalPathsToScan is just like pathsToScan, but the paths are permitted to be absent.
	optionalPathsToScan = []string{
		// Gone in 10.15.
		"/Library/QuickTime",
		// Not present in dumped dyld_shared_caches
		"/System/Library/Components",
	}

	// uploadServersV1 are the list of servers to which symbols should be
	// uploaded when using the V1 protocol.
	uploadServersV1 = []string{
		"https://clients2.google.com/cr/symbol",
		"https://clients2.google.com/cr/staging_symbol",
	}
	// uploadServersV2 are the list of servers to which symbols should be
	// uploaded when using the V2 protocol.
	uploadServersV2 = []string{
		"https://staging-crashsymbolcollector-pa.googleapis.com",
		"https://prod-crashsymbolcollector-pa.googleapis.com",
	}

	// uploadServers are the list of servers that should be used, accounting
	// for whether v1 or v2 protocol is used.
	uploadServers = uploadServersV1

	// blacklistRegexps match paths that should be excluded from dumping.
	blacklistRegexps = []*regexp.Regexp{
		regexp.MustCompile(`/System/Library/Frameworks/Python\.framework/`),
		regexp.MustCompile(`/System/Library/Frameworks/Ruby\.framework/`),
		regexp.MustCompile(`_profile\.dylib$`),
		regexp.MustCompile(`_debug\.dylib$`),
		regexp.MustCompile(`\.a$`),
		regexp.MustCompile(`\.dat$`),
	}
	maxFileCreateTries = 10
)

func main() {
	flag.Parse()
	log.SetFlags(0)

	// If `apiKey` is set, we're using the v2 protocol.
	if len(*apiKey) > 0 {
		uploadServers = uploadServersV2
	}

	var uq *UploadQueue

	if *uploadOnlyPath != "" {
		// -upload-from specified, so handle that case early.
		uq = StartUploadQueue()
		uploadFromDirectory(*uploadOnlyPath, uq)
		uq.Wait()
		return
	}

	dumpPath := *dumpOnlyPath
	if *dumpOnlyPath == "" {
		// If -dump-to was not specified, then run the upload pipeline and create
		// a temporary dump output directory.
		uq = StartUploadQueue()

		if p, err := os.MkdirTemp("", "upload_system_symbols"); err != nil {
			log.Fatalf("Failed to create temporary directory: %v", err)
		} else {
			dumpPath = p
			defer os.RemoveAll(p)
		}
	}

	tempDir, err := os.MkdirTemp("", "systemRoots")
	if err != nil {
		log.Fatalf("Failed to create temporary directory: %v", err)
	}
	defer os.RemoveAll(tempDir)
	roots := getSystemRoots(tempDir)

	if *dumpOnlyPath != "" {
		// -dump-to specified, so make sure that the path is a directory.
		if fi, err := os.Stat(*dumpOnlyPath); err != nil {
			log.Fatalf("-dump-to location: %v", err)
		} else if !fi.IsDir() {
			log.Fatal("-dump-to location is not a directory")
		}
	}

	dq := StartDumpQueue(roots, dumpPath, *separateArch, uq)
	dq.Wait()
	if uq != nil {
		uq.Wait()
	}
}

// getSystemRoots returns which system roots should be dumped from the parsed
// flags, extracting them if necessary.
func getSystemRoots(tempDir string) []string {
	hasInstaller := len(*installer) > 0
	hasIPSW := len(*ipsw) > 0
	hasRoot := len(*systemRoot) > 0

	if hasInstaller {
		if hasIPSW || hasRoot {
			log.Fatalf("--installer, --ipsw, and --system-root are mutually exclusive")
		}
		if rs, err := extractSystems(archive.Installer, *installer, tempDir); err != nil {
			log.Fatalf("Couldn't extract installer at %s: %v", *installer, err)
		} else {
			return rs
		}
	} else if hasIPSW {
		if hasRoot {
			log.Fatalf("--installer, --ipsw, and --system-root are mutually exclusive")
		}
		if rs, err := extractSystems(archive.IPSW, *ipsw, tempDir); err != nil {
			log.Fatalf("Couldn't extract IPSW at %s: %v", *ipsw, err)
		} else {
			return rs
		}
	} else if hasRoot {
		return []string{*systemRoot}
	}
	log.Fatal("Need a --system-root, --installer, or --ipsw to dump symbols for")
	return []string{}
}

// manglePath reduces an absolute filesystem path to a string suitable as the
// base for a file name which encodes some of the original path. The result
// concatenates the leading initial from each path component except the last to
// the last path component; for example /System/Library/Frameworks/AppKit
// becomes SLFAppKit.
// Assumes ASCII.
func manglePath(path string) string {
	components := strings.Split(path, "/")
	n := len(components)
	builder := strings.Builder{}
	for i, component := range components {
		if len(component) == 0 {
			continue
		}
		if i < n-1 {
			builder.WriteString(component[:1])
		} else {
			builder.WriteString(component)
		}
	}
	return builder.String()
}

// createSymbolFile creates a writable file in `base` with a name derived from
// `original_path`. It ensures that multiple threads can't simultaneously create
// the same file for two `original_paths` that map to the same mangled name.
// Returns the filename, the file, and an error if creating the file failed.
func createSymbolFile(base string, original_path string, arch string) (filename string, f *os.File, err error) {
	mangled := manglePath(original_path)
	counter := 0
	filebase := path.Join(base, mangled)
	for {
		var symfile string
		if counter == 0 {
			symfile = fmt.Sprintf("%s_%s.sym", filebase, arch)
		} else {
			symfile = fmt.Sprintf("%s_%s_%d.sym", filebase, arch, counter)
		}
		f, err := os.OpenFile(symfile, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
		if err == nil {
			return symfile, f, nil
		}
		if os.IsExist(err) && counter < maxFileCreateTries {
			counter++
			continue
		}
		return "", nil, err
	}

}

type WorkerPool struct {
	wg sync.WaitGroup
}

// StartWorkerPool will launch numWorkers goroutines all running workerFunc.
// When workerFunc exits, the goroutine will terminate.
func StartWorkerPool(numWorkers int, workerFunc func()) *WorkerPool {
	p := new(WorkerPool)
	for i := 0; i < numWorkers; i++ {
		p.wg.Add(1)
		go func() {
			workerFunc()
			p.wg.Done()
		}()
	}
	return p
}

// Wait for all the workers in the pool to complete the workerFunc.
func (p *WorkerPool) Wait() {
	p.wg.Wait()
}

type UploadQueue struct {
	*WorkerPool
	queue chan string
}

// StartUploadQueue creates a new worker pool and queue, to which paths to
// Breakpad symbol files may be sent for uploading.
func StartUploadQueue() *UploadQueue {
	uq := &UploadQueue{
		queue: make(chan string, 10),
	}
	uq.WorkerPool = StartWorkerPool(5, uq.worker)
	return uq
}

// Upload enqueues the contents of filepath to be uploaded.
func (uq *UploadQueue) Upload(filepath string) {
	uq.queue <- filepath
}

// Done tells the queue that no more files need to be uploaded. This must be
// called before WorkerPool.Wait.
func (uq *UploadQueue) Done() {
	close(uq.queue)
}

func (uq *UploadQueue) runSymUpload(symfile, server string) *exec.Cmd {
	symUpload := path.Join(*breakpadTools, "symupload")
	args := []string{symfile, server}
	if len(*apiKey) > 0 {
		args = append([]string{"-p", "sym-upload-v2", "-k", *apiKey}, args...)
	}
	return exec.Command(symUpload, args...)
}

func (uq *UploadQueue) worker() {
	for symfile := range uq.queue {
		for _, server := range uploadServers {
			for i := 0; i < 3; i++ { // Give each upload 3 attempts to succeed.
				cmd := uq.runSymUpload(symfile, server)
				if output, err := cmd.Output(); err == nil {
					// Success. No retry needed.
					fmt.Printf("Uploaded %s to %s\n", symfile, server)
					break
				} else if exitError, ok := err.(*exec.ExitError); ok && exitError.ExitCode() == 2 && *apiKey != "" {
					// Exit code 2 in protocol v2 means the file already exists on the server.
					// No point retrying.
					fmt.Printf("File %s already exists on %s\n", symfile, server)
					break
				} else {
					log.Printf("Error running symupload(%s, %s), attempt %d: %v: %s\n", symfile, server, i, err, output)
					time.Sleep(1 * time.Second)
				}
			}
		}
	}
}

type DumpQueue struct {
	*WorkerPool
	dumpPath     string
	queue        chan dumpRequest
	separateArch bool
	uq           *UploadQueue
}

type dumpRequest struct {
	path string
	arch string
}

// StartDumpQueue creates a new worker pool to find all the Mach-O libraries in
// root and dump their symbols to dumpPath. If an UploadQueue is passed, the
// path to the symbol file will be enqueued there, too.
func StartDumpQueue(roots []string, dumpPath string, separateArch bool, uq *UploadQueue) *DumpQueue {
	dq := &DumpQueue{
		dumpPath:     dumpPath,
		queue:        make(chan dumpRequest),
		separateArch: separateArch,
		uq:           uq,
	}
	dq.WorkerPool = StartWorkerPool(12, dq.worker)

	findLibsInRoots(roots, dq)

	return dq
}

// DumpSymbols enqueues the filepath to have its symbols dumped in the specified
// architecture.
func (dq *DumpQueue) DumpSymbols(filepath string, arch string) {
	dq.queue <- dumpRequest{
		path: filepath,
		arch: arch,
	}
}

func (dq *DumpQueue) Wait() {
	dq.WorkerPool.Wait()
	if dq.uq != nil {
		dq.uq.Done()
	}
}

func (dq *DumpQueue) done() {
	close(dq.queue)
}

func (dq *DumpQueue) worker() {
	dumpSyms := path.Join(*breakpadTools, "dump_syms")

	for req := range dq.queue {
		dumpPath := dq.dumpPath
		if dq.separateArch {
			dumpPath = path.Join(dumpPath, req.arch)
			if err := ensureDirectory(dumpPath); err != nil {
				log.Fatalf("Error creating directory %s: %v", dumpPath, err)
			}
		}
		symfile, f, err := createSymbolFile(dumpPath, req.path, req.arch)
		if err != nil {
			log.Fatalf("Error creating symbol file: %v", err)
		}

		cmd := exec.Command(dumpSyms, "-a", req.arch, req.path)
		cmd.Stdout = f
		err = cmd.Run()
		f.Close()

		if err != nil {
			os.Remove(symfile)
			log.Printf("Error running dump_syms(%s, %s): %v\n", req.arch, req.path, err)
		} else if dq.uq != nil {
			dq.uq.Upload(symfile)
		}
	}
}

// uploadFromDirectory handles the upload-only case and merely uploads all files in
// a directory.
func uploadFromDirectory(directory string, uq *UploadQueue) {
	d, err := os.Open(directory)
	if err != nil {
		log.Fatalf("Could not open directory to upload: %v", err)
	}
	defer d.Close()

	entries, err := d.Readdirnames(0)
	if err != nil {
		log.Fatalf("Could not read directory: %v", err)
	}

	for _, entry := range entries {
		uq.Upload(path.Join(directory, entry))
	}

	uq.Done()
}

// findQueue is an implementation detail of the DumpQueue that finds all the
// Mach-O files and their architectures.
type findQueue struct {
	*WorkerPool
	queue chan string
	dq    *DumpQueue
}

// findLibsInRoot looks in all the pathsToScan in all roots and manages the
// interaction between findQueue and DumpQueue.
func findLibsInRoots(roots []string, dq *DumpQueue) {
	fq := &findQueue{
		queue: make(chan string, 10),
		dq:    dq,
	}
	fq.WorkerPool = StartWorkerPool(12, fq.worker)
	for _, root := range roots {
		for _, p := range pathsToScan {
			fq.findLibsInPath(path.Join(root, p), true)
		}

		for _, p := range optionalPathsToScan {
			fq.findLibsInPath(path.Join(root, p), false)
		}
	}

	close(fq.queue)
	fq.Wait()
	dq.done()
}

// findLibsInPath recursively walks the directory tree, sending file paths to
// test for being Mach-O to the findQueue.
func (fq *findQueue) findLibsInPath(loc string, mustExist bool) {
	d, err := os.Open(loc)
	if err != nil {
		if !mustExist && os.IsNotExist(err) {
			return
		}
		log.Fatalf("Could not open %s: %v", loc, err)
	}
	defer d.Close()

	for {
		fis, err := d.Readdir(100)
		if err != nil && err != io.EOF {
			log.Fatalf("Error reading directory %s: %v", loc, err)
		}

		for _, fi := range fis {
			fp := path.Join(loc, fi.Name())
			if fi.IsDir() {
				fq.findLibsInPath(fp, true)
				continue
			} else if fi.Mode()&os.ModeSymlink != 0 {
				continue
			}

			// Test the blacklist in the worker to not slow down this main loop.

			fq.queue <- fp
		}

		if err == io.EOF {
			break
		}
	}
}

func (fq *findQueue) worker() {
	for fp := range fq.queue {
		blacklisted := false
		for _, re := range blacklistRegexps {
			blacklisted = blacklisted || re.MatchString(fp)
		}
		if blacklisted {
			continue
		}

		f, err := os.Open(fp)
		if err != nil {
			log.Printf("%s: %v", fp, err)
			continue
		}

		fatFile, err := macho.NewFatFile(f)
		if err == nil {
			// The file is fat, so dump its architectures.
			for _, fatArch := range fatFile.Arches {
				fq.dumpMachOFile(fp, fatArch.File)
			}
			fatFile.Close()
		} else if err == macho.ErrNotFat {
			// The file isn't fat but may still be MachO.
			thinFile, err := macho.NewFile(f)
			if err != nil {
				log.Printf("%s: %v", fp, err)
				continue
			}
			fq.dumpMachOFile(fp, thinFile)
			thinFile.Close()
		} else {
			f.Close()
		}
	}
}

func (fq *findQueue) dumpMachOFile(fp string, image *macho.File) {
	if image.Type != arch.MachODylib &&
		image.Type != arch.MachOBundle &&
		image.Type != arch.MachODylinker {
		return
	}

	arch := arch.GetArchStringFromHeader(image.FileHeader)
	if arch == "" {
		// Don't know about this architecture type.
		return
	}

	if (*dumpArchitecture != "" && *dumpArchitecture == arch) || *dumpArchitecture == "" {
		fq.dq.DumpSymbols(fp, arch)
	}
}

// extractSystems extracts any dyld shared caches from `archivePath`, then extracts the caches
// into macOS system libraries, returning the locations on disk of any systems extracted.
func extractSystems(format archive.ArchiveFormat, archivePath string, extractPath string) ([]string, error) {
	cachesPath := path.Join(extractPath, "caches")
	if err := os.MkdirAll(cachesPath, 0755); err != nil {
		return nil, err
	}
	if err := archive.ExtractCaches(format, archivePath, cachesPath, true); err != nil {
		return nil, err
	}
	files, err := os.ReadDir(cachesPath)
	if err != nil {
		return nil, err
	}
	cachePrefix := "dyld_shared_cache_"
	extractedDirPath := path.Join(extractPath, "extracted")
	roots := make([]string, 0)
	for _, file := range files {
		fileName := file.Name()
		if filepath.Ext(fileName) == "" && strings.HasPrefix(fileName, cachePrefix) {
			arch := strings.TrimPrefix(fileName, cachePrefix)
			extractedSystemPath := path.Join(extractedDirPath, arch)
			// XXX: Maybe this shouldn't be fatal?
			if err := extractDyldSharedCache(path.Join(cachesPath, fileName), extractedSystemPath); err != nil {
				return nil, err
			}
			roots = append(roots, extractedSystemPath)
		}
	}
	return roots, nil
}

// extractDyldSharedCache extracts the dyld shared cache at `cachePath` to `destination`.
func extractDyldSharedCache(cachePath string, destination string) error {
	dscExtractor := path.Join(*breakpadTools, "dsc_extractor")
	cmd := exec.Command(dscExtractor, cachePath, destination)
	if output, err := cmd.Output(); err != nil {
		return fmt.Errorf("extracting shared cache at %s: %v. dsc_extractor said %v", cachePath, err, output)
	}
	return nil
}

// ensureDirectory creates a directory at `path` if one does not already exist.
func ensureDirectory(path string) error {
	if _, err := os.Stat(path); errors.Is(err, os.ErrNotExist) {
		return os.MkdirAll(path, 0755)
	} else {
		return err
	}
}
