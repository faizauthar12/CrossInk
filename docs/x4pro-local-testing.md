# Local X4 Pro firmware testing

CrossInk owns the build, flash composition, fixtures, assertions and evidence.
The emulator is an external dependency, initially pinned to
[`serialx/x4prosim` `55f6917885d89b60834cf8987177813e9fef648c`](https://github.com/serialx/x4prosim/tree/55f6917885d89b60834cf8987177813e9fef648c).
This supplements the fast native CMake/CTest, sanitizer and SDL simulator tests.
Keep those tests as the first checks for parser, store and resource changes.

## Current acceptance status

On macOS arm64, the pinned native and browser runtimes boot CrossInk X4 Pro.
The native suite passes Home, synthetic EPUB opening/page-turn and a settings
change on source `6435493600de5e54a0ab823d884b4d76591bab75`, SDK
`fce2403b5b52042bf819e4b20b6bbcb16c9dfdaf`, using `x4-pro-debug`.
Installing the SD test language prepares generation 1, then the restarted
firmware asserts in ESP-IDF `spi_flash_munmap`. A fresh QEMU process with copies
of that flash/SD reproduces it. The decoded call chain is `I18n::begin()`
(`lib/I18n/I18n.cpp:103`) -> `HalFlashPartition::unmap()`
(`lib/HalFlashPartition/HalFlashPartition.cpp:137`) -> `esp_partition_munmap()`
-> `spi_flash_munmap()`. The diagnostic says the unmap pointer is outside
external memory regions. Firmware versus emulator cause is not established.

The five-scenario command intentionally fails on this combination. It does not
hide the crash or claim installation/restart acceptance. Resolve the mapping
compatibility failure before making this a CI gate. The complete suite passed
all five scenarios on an older local X4 Pro debug image, whose logged identity
was `1.6-next-development+8cd48`. Its exact build provenance is unavailable, so
that result validates the controls and assertions but does not validate this base.
Read-only comparison found no changes to the language mapping/unmapping code
between those versions. The newer base adds the PSRAM startup fallback in
`lib/hal/HalSystem.cpp:244`; this is an investigation lead, not an established
cause. A read-only GDB attempt could not inspect the live mapping because the
installed debugger and QEMU disagree on the remote register packet layout.

## Trusted setup on this Mac

The initial locally verified runtime is `/tmp/crossink-x4pro-runtime/qemu`.
The browser package is `/tmp/crossink-x4prosim-wasm.zip`. Temporary paths can be
removed by the OS; preserve approved artifacts somewhere durable if needed.
`scripts/x4pro_local/provenance.json` records the full source pin, successful
GitHub Actions run `38066668497`, artifact IDs and SHA-256 digests:

| Package | Artifact ID | Archive SHA-256 |
| --- | --- | --- |
| macOS arm64 native | `11675089686` | `b8b009973b6e8476a236112838929b1e41ba4fc1ed2b47d211c54a5753b611e8` |
| browser WASM | `11675735170` | `6441c23cc4e6e4d66c27d5f271961a748b8a5656c66f7aee2d20a0661ca474f9` |

The native executable SHA-256 is
`84845734fae320f1324b6f0cc91c8c710b151eddc6a4b70c55304460a523aff4`.
The runner checks it before launch and checks the browser archive before
extracting. Download only the artifacts of the recorded successful run, verify
their digests, inspect the source and request any required execution approval
before running newly obtained code. The runner never downloads or installs.
Do not replace a denied installation with a bundled library, rebuild, trust
change, disabled certificate check or quarantine removal.

Required host tools are Python 3.9+, `mcopy`, `mtype`, `mkfs.fat`, and the approved
emulator. The native artifact links Homebrew GLib, Pixman, libpng, SDL2,
libslirp and libgcrypt. Existing trusted libraries were reused; the approved
additional installations were Homebrew core `libslirp 4.9.5`, `libgcrypt 1.12.4`,
`mtools 4.0.49`, `dosfstools 4.2`, and dependency `libgpg-error 1.61`.
Browser probing uses existing Node 26.11.1 (Node 22+ provides WebSocket) and
Chrome. Neither npm packages nor a new browser install are needed.

## Build and compose CrossInk firmware

Initialize an isolated firmware checkout with `git submodule update --init
--recursive`. Coordinate exclusive use of the S3 PlatformIO core before building.
Use the S3 wrapper; never run it concurrently with another direct S3 build.
The build helper locks cooperating calls, selects exactly one debug environment,
limits jobs to one, bounds build time and records the source, SDK and artifact
hashes. It fingerprints tracked/untracked firmware inputs, build scripts,
web/assets/board sources and ignored `platformio.local.ini` overrides, plus
the resolved PlatformIO configuration hash. The effective configuration is
hashed rather than copied into the manifest, so values in local overrides are
not disclosed. Source/configuration changes during the build reject attribution.
It does not change the source or flash hardware:

```sh
python3 scripts/x4pro_local/build.py \
  --source /tmp/crossink-x4pro-local-testing \
  --pio /Users/home/.local/bin/codex-pio-s3 \
  --log /tmp/crossink-x4pro-build.log
```

The designated `/Users/home/code/crossink-build-s3` checkout had unrelated
changes during initial setup. The initial build therefore used the isolated
checkout and the same trusted S3 wrapper/core, leaving that checkout untouched.
The helper is explicit about `--source`, so another isolated checkout can be
selected. Inspect cached dependencies first and obtain required approval before
letting PlatformIO install anything missing. Do not reuse the C3 package core.

Every run takes `--build-dir` explicitly. It can point at a separately built
feature image, including an existing language-feature worktree; no merge or
edit is needed. Native manual readiness and all scenario predicates require
the supported `x4-pro-debug` configuration (`LOG_LEVEL=2`), because activity-entry
messages are debug logs. The runner checks for their format-string signature
in the application before launching QEMU. An image without it exits `2`, with
manifest status `unsupported-image` and `firmware_started: false`; this is a
log-profile incompatibility, not a firmware crash or a failed Home boot.
Signature presence is a preflight check; the actual runtime predicates remain
unchanged and still must pass. Browser manual UI can use a production image,
but the optional CDP Home assertion also requires the debug messages.
A build record, when available, is included in the run manifest.
Without a record, hashes identify the input bytes but do not prove what source
produced them. A supplied record must contain matching SHA-256 values for all
three composed binaries; missing or stale hashes reject source attribution and
composition. `build_record_verification: artifact-hashes-match` verifies the
record's relationship to those bytes, not an independent signature of its
source claim. Keep the ELF beside the binaries for backtrace decoding.

The composer uses that build's `bootloader.bin` at `0`, `partitions.bin` at
`0x8000`, and `firmware.bin` at `0x10000`, leaving NVS and OTA metadata erased.
It checks ESP32-S3 headers, boot-asset bounds and app0 capacity, then creates a
complete 16 MiB image. Use X4 Pro firmware; a chip ID alone does not identify a
board or prove Sticky compatibility. Native readiness also checks the logged
`xteink_x4_pro` board. Browser receives the complete image, preserving CrossInk's
own bootloader and partitions rather than invoking CrossPoint bare-app composition.

## One manual command

```sh
python3 scripts/x4pro_local/run.py manual \
  --runtime /tmp/crossink-x4pro-runtime/qemu \
  --build-dir /tmp/crossink-x4pro-local-testing/.pio/build/x4-pro-debug \
  --fixture sd-assets
```

This opens native SDL with keyboard Up/Down/Power and pointer/touch controls.
Ctrl-C, SIGTERM and SIGHUP terminate the owned process group. Default duration is one hour;
`--overall-timeout` changes it. Native firmware logs and private QMP socket
details appear in the new run directory. Use structured JSON QMP `qom-set` for
`/machine/x4pro-keys` (`up`, `down`, `power` booleans), `/machine/gt911` (`tap`
string `X,Y` or `home`), `screendump`, `system_reset` and `quit`. The Python
`Native`/`QMP` classes expose these controls for custom assertion scripts;
do not use `drive.py` or shared `.run` images. QMP replies are matched by ID;
asynchronous reset/shutdown events are retained in the event log.

For browser manual use, change the backend of the same command:

```sh
python3 scripts/x4pro_local/run.py manual --backend browser \
  --web-archive /tmp/crossink-x4prosim-wasm.zip \
  --build-dir /tmp/crossink-x4pro-local-testing/.pio/build/x4-pro-debug \
  --fixture sd-assets --cdp-port 0
```

The runner creates a fresh Chrome profile and an OS-assigned localhost HTTP port,
sets COOP/COEP, uses `wifi=off`, explicitly selects `turbo=0`, and serves only
local private copies. CDP is optional; `0` requests an automatic dedicated port.
The browser URL and profile are recorded in `browser.json`. For an optional
assertion, console capture and screenshot of this private tab:

```sh
node scripts/x4pro_local/browser_probe.mjs /absolute/path/to/run-directory 90
```

The probe uses CrossInk Home entry followed by panel completion, independent of
the fork's CrossPoint `[MEM]` UI status. Download the SD image through the browser
before closing if guest writes are wanted as an artifact. Browser SD is profile
persistent; flash writes are ephemeral. Editing card files resets the guest.
Installation and flash-persistence assertions therefore run natively.
Timing is the emulator's model timing, not a measured hardware performance result.

The WASM HTTP adapter accepts untrusted guest TLS and strips Authorization.
Keep it offline here. Future real HTTPS tests should use native QEMU's Wi-Fi/NAT
with separate explicit authorization; retain CrossInk certificate validation.
This workflow uses `-nic none`, adds no port forwards and changes no network,
certificate, browser security or system permissions. Never upload private
firmware to the hosted emulator.

## One agent test command

```sh
python3 scripts/x4pro_local/run.py test \
  --runtime /tmp/crossink-x4pro-runtime/qemu \
  --build-dir /tmp/crossink-x4pro-local-testing/.pio/build/x4-pro-debug
```

The suite uses a fresh `sd-assets` card, Lyra portrait layout, Small UI scale,
English fallback labels and default text antialiasing. It asserts:

1. CrossInk X4 Pro Home entry and a completed panel refresh.
2. Open original synthetic `TestBook.epub`, render a 32-page chapter, turn one
   page, leave the reader and verify saved spine `0`, page `1`, and page count.
3. Change Display > Dark Mode and verify `screenInverted: 1` in saved SD JSON.
4. Install `Local Test` language from SD and verify `ZZ-TEST`, generation `1`
   after restart; install the French hyphenation pack and verify one mapped pack.
5. Reset the machine, verify settings/language/pack and book progress persist,
   then reopen the book from its cache and recheck progress.

Default readiness/action/overall timeouts are 90/30/600 seconds. Override with
`--ready-timeout`, `--action-timeout`, `--overall-timeout`. Navigation waits on
activity and panel logs. EPUB rendering waits through grayscale completion and
framebuffer restoration. Small key-hold intervals model input; arbitrary boot
sleeps are not success predicates. Firmware panic/assert/watchdog signatures,
unexpected process exit and unmet assertions fail the command. A crash is
detected before readiness is accepted. A failed run keeps the final panel and
all available evidence and terminates the process; it never marks unexecuted
scenarios passed. Tests do not rewrite or preinstall the expected settings.

Named disposable fixtures are `blank-home`, `reading`, and `sd-assets`. The
synthetic EPUB text is CC0; French patterns come from CrossInk's maintained
snapshot and retain its notices in `assets/hyphenation`. No private books or
credentials are used. Fixture timestamps and FAT metadata are fixed, so repeated
generation with the recorded host tools produces the same seed bytes. Manual
`--sd /path/to/seed.img` copies an explicit seed. The five-scenario suite requires
its known `sd-assets` fixture. To replay a retained seed, add
`--sd /path/to/run/fixture/sd.img`; its adjacent `fixture.json` must name
`sd-assets` and match the unchanged seed hash. Do not use a guest-modified card
for fresh-fixture acceptance.

Each run prints a new private directory under `/tmp/crossink-x4pro-runs`:
`manifest.json` contains input hashes, source/build references, pin, timeouts,
results and scenario status; `events.jsonl` holds QMP requests/replies;
`serial.log`, `stdout.log`, `stderr.log`, panel PNGs, final `flash.bin` and
`sd.img` remain. Native teardown extracts `.crosspoint` into `sd-results` and
records extraction status. The input SD seed is rehashed after the run.
Short private Unix socket paths avoid macOS path limits. No shared flash, card,
HTTP port or browser profile is reused. The owned process group is terminated,
waited for and killed if necessary; socket directories are removed. Final panel
capture and graceful QMP shutdown have a separate five-second cleanup budget
after an expired run deadline. SIGKILL or host failure cannot run cleanup;
use the recorded PID/process group for recovery in those cases.
Teardown checks the owned process group independently of its leader and kills
surviving descendants, including a child that ignores SIGTERM after the leader
exits. The same bounded group cleanup is shared by QEMU, Chrome and build runs.

Host runner contracts can be checked without firmware:

```sh
python3 -m unittest discover -s scripts/x4pro_local -p 'test_*.py' -v
```

These tests include a repository-owned stub QMP process to check SIGTERM,
SIGHUP and deadline cleanup, so sandboxed execution may require approval to
bind its temporary local Unix socket.

For hardware comparison, on a separately authorized X4 Pro use the same fresh
SD fixture files: open/turn/close TestBook, toggle Dark Mode, install Local Test
under System > Device > Language and French under Reader > Page Layout >
Hyphenation Packs, then restart and reopen. Expect page two, saved Dark Mode,
`Mapped ZZ-TEST` and `Mapped 1 packs`, with no MMU/assert diagnostic. Fresh cards
need no cache reset; if reusing a card, clear only this book's `epub_<path-hash>`
cache when validating layout changes. Hardware flashing is outside this workflow.
