# CLAUDE.md

Guidance for working in this repo. See `README.md` (operator guide) and
`DESIGN.md` (architecture contract) for the project itself.

## Build & test locally

```sh
cmake -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.16 and **DCMTK ≥ 3.7.0** (see the floor note below).

Tests (`add_test` in `CMakeLists.txt`):
- `dicomq-unit-profile`, `dicomq-unit-spool` — pure helpers, no DICOM
  networking. Always runnable.
- `dicomq-integration` — drives the real binaries against a throwaway
  spool, including network legs that shell out to DCMTK's
  `storescu`/`storescp`/`dcmdump`/`dump2dcm`. The network legs **skip**
  cleanly when those tools are not on `PATH`, and need loopback
  networking (they fail in a sandbox without it).

## Linters

CI enforces both, pinned to **LLVM/clang 21** to match local dev. Run
them the same way CI does:

```sh
# format (pinned 21.1.8; uvx fetches it on demand)
uvx clang-format@21.1.8 --dry-run --Werror $(git ls-files 'src/*.cc' 'src/*.h' 'test/*.cc')

# tidy (needs build/compile_commands.json from a configure)
clang-tidy -p build --warnings-as-errors='*' $(git ls-files 'src/*.cc')
```

`.clang-tidy` is the baseline. It enables `clang-analyzer-*`,
`bugprone-*`, `performance-*`, `portability-*` wildcards and disables a
handful of noisy checks — including `bugprone-exception-escape` (it flags
every `main()` for a `std::string` that can throw, visible only under
libc++). Keep linters on clang 21: clang-tidy 22 enables new checks
(e.g. `bugprone-throwing-static-initialization` on the `static Spool sp;`
globals) that the codebase has not been vetted against.

## CI workflows (`.github/workflows/`)

Three workflows, all on push / PR / manual dispatch:

- **`lint.yml`** — `ubuntu-latest`, clang-format only (no DCMTK needed,
  kept fast).
- **`macos.yml`** — `macos-14` (arm64). Full build + `ctest`, clang-tidy
  with `--warnings-as-errors`, then bundles dylibs into a self-contained
  artifact. DCMTK comes from Homebrew.
- **`linux.yml`** — `ubuntu-latest`. Builds DCMTK from source, then full
  build + `ctest`. This is the only job that exercises the Linux-only
  `inotify` scan path in `dicomq-send` (macOS falls back to periodic
  scanning).

### Non-obvious CI facts (read before editing the workflows)

- **DCMTK 3.7.0 floor.** `CMakeLists.txt` requires DCMTK 3.7.0
  (CVE-2025-14607 fix). Ubuntu's apt package is older (3.6.7), so the
  Linux job **builds DCMTK from source**; apt's `dcmtk` is installed only
  for the test-peer tools (`storescu`/`storescp`/`dcmdump`/`dump2dcm`),
  whose version is irrelevant.
- **Build only the modules dicomq links.** The Linux DCMTK build passes
  `-DDCMTK_MODULES="ofstd;oflog;oficonv;dcmdata;dcmimgle;dcmimage;dcmjpeg;dcmjpls;dcmnet;dcmtls"`.
  `BUILD_APPS=OFF` alone does **not** stop module compilation — without
  `DCMTK_MODULES` the build also compiles `dcmrt` (hundreds of files),
  `dcmiod`, `dcmwlm`, `dcmqrdb`, … and times out. This list is the
  closure of what dicomq links; if a link target is added, extend it.
- **DCMTK build is cached** keyed on version + module set, via
  `cache/restore` + `cache/save` (`if: always()`) so it survives even a
  later step failing. Bump the cache key whenever `DCMTK_VERSION` or the
  module list changes.
- **macOS clang-tidy is pinned to `llvm@21`** and passed
  `-isysroot $(xcrun --show-sdk-path)` — brew's clang-tidy does not
  inherit AppleClang's implicit macOS SDK header search and otherwise
  fails with `'<string>' file not found`.
- A `timeout-minutes` + `concurrency` group guard the Linux job so a
  runaway build fails fast and rapid pushes cancel superseded runs.

## Commits / pushing

- Branch off `main`; do not commit directly to it.
- This box pushes over an SSH deploy key (`git push`); never run
  `gh auth login` here, and `gh` API calls are unauthenticated.
