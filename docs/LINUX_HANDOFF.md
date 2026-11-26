# Linux desktop parity handoff

This document records the Linux work at the handoff point and the steps needed to continue it on a fresh x86_64 Ubuntu laptop. It describes a work in progress, not a released parity claim.

Read [docs/UPSTREAM_CONTRIBUTING.md](UPSTREAM_CONTRIBUTING.md) before preparing changes for upstream. The player-facing behavior is in [../linux/README.md](../linux/README.md) and [manual/linux.md](manual/linux.md). Core test details are in [../libretro/test/README.md](../libretro/test/README.md).

## Repository and starting point

- `origin` (fork): https://github.com/SneezeGUI/pokedaisy
- `upstream`: https://github.com/lidor30/pokedaisy
- Branch: `linux-desktop-parity`
- Historical base commit: `fc5048dda9d3c1b5947e176ad8a6e8a39b2d0d8e`
- The integration work began as tracked and untracked changes on that base. Commit `3a310f5d` contains that integration work. The `linux-desktop-parity` branch on the fork is authoritative for the integrated source; use the patch method below only for later uncommitted work.

The verification machine was an x86_64 Ubuntu 24.04 laptop with an Intel Core i5-8250U, Intel UHD Graphics 620 and 8 GB of RAM. This is the reference machine used for this round, **not a minimum or recommended specification**. It does not establish performance or compatibility on other hardware.

The examples below use `~/builds/pokedaisy` for the existing checkout, `~/src/pokedaisy` for the new checkout, `~/Applications/PokeDaisy-wip` for an installed build and `~/PokeDaisy` for player data. These are conventions for the handoff. The program does not require an XDG data directory.

## Clone the published branch

Use the published fork branch for a fresh handoff:

```sh
git clone --branch linux-desktop-parity \
  https://github.com/SneezeGUI/pokedaisy.git ~/src/pokedaisy
cd ~/src/pokedaisy
git remote add upstream https://github.com/lidor30/pokedaisy.git
git fetch origin upstream
git rev-parse HEAD
git status --short --branch
```

The branch contains both the tracked edits and the files that were untracked during development. Do not apply the original pre-publication patch or untracked archive to this checkout. Confirm that `HEAD` is the expected handoff commit and initialize the pinned submodules as described below.

For an existing clone, fetch and update without overwriting local work:

```sh
git fetch origin
git checkout linux-desktop-parity
git pull --ff-only origin linux-desktop-parity
```

Use the original patch and untracked archive only to reconstruct work that has not yet been committed. Do not apply them over the published integration branch.

## Preserve later uncommitted WIP

Use this method only for new dirty work. A normal `git diff` omits untracked files, so preserve both parts. Record the base revision, inspect every file list, and do not put ROMs, saves, states, build outputs, local configuration or secrets in either archive.

On the machine with uncommitted work:

```sh
cd ~/builds/pokedaisy
git status --short
git rev-parse HEAD > ~/pokedaisy-linux-wip-base.txt
git diff --binary > ~/pokedaisy-linux-wip.patch
git ls-files --others --exclude-standard
git ls-files --others --exclude-standard -z | \
  tar --null -czf ~/pokedaisy-linux-wip-untracked.tar.gz --files-from=-
tar -tzf ~/pokedaisy-linux-wip-untracked.tar.gz
sha256sum ~/pokedaisy-linux-wip-base.txt ~/pokedaisy-linux-wip.patch \
  ~/pokedaisy-linux-wip-untracked.tar.gz
```

Review the patch and archive listing. The untracked list should contain only intended source, test and documentation files. Transfer the archives, base record and checksums privately.

On the receiving machine, first clone or fetch the intended base. For WIP based on the published integration branch, use the published branch, not the historical base commit. Confirm the checkout is clean and its `HEAD` matches the recorded base, then apply the files:

```sh
cd ~/src/pokedaisy
git status --short
git rev-parse HEAD
git apply --check ~/pokedaisy-linux-wip.patch
git apply ~/pokedaisy-linux-wip.patch
tar -xzf ~/pokedaisy-linux-wip-untracked.tar.gz
git status --short
git diff --check
```

Keep these archives with any package made from the dirty tree. Once the work is committed and pushed, the new commit and the published branch replace the archives as the normal handoff mechanism.

## Scope of the local changes

`git status --short` and `git diff` are the authoritative file-level inventory. The current work has these parts:

- **Host input:** `linux/host_input.c` and `linux/main.c` add keyboard and SDL controller remapping, controller triggers, conflict handling, hotplug behavior, separate DS hotkeys, state-slot hotkeys and undo hotkeys. Bindings persist in `host.ini`.
- **Host state and saves:** `linux/host_state.c` and `linux/main.c` add ten state slots, thumbnails, atomic writes, save/load undo, guarded automatic resume and `--import-save` / `--restore-save`. Battery backups and resume invalidation are part of the save workflow.
- **Display:** `linux/display_shader.c` implements `OFF`, `LCD`, `SCANLINES`, `CRT`, `GBA COLOR` and `EDGE SMOOTH`. Filter and thumbnail code reads the callback-owned frame copy, with bounded intermediate buffers.
- **Library save tools:** `linux/save_tools.c`, `linux/save_tools_ui.c` and `linux/library.c` add Gen 3 save editing and two-save trading to `TOOLS`, with backups, stale-file rejection, atomic replacement, read-back checks, rollback and resume invalidation. The supported scope is retail English and European FireRed, LeafGreen, Ruby, Sapphire and Emerald. Admission uses ROM-header and save-layout checks, not a retail SHA allowlist, and unsupported layouts are rejected. Synthetic tests do not validate every advertised family or European language, so unexercised variants remain unverified.
- **Shared gameplay:** new `3ds/core/pd_center.*` and `3ds/core/pd_party.*`, plus `libretro/pd_lr_gameplay.*` and `libretro/pd_lr_mem.*`, carry Center services, party switching with rollback, delayed trade evolution, queued game calls, battle actions, reusable-TM and instant-text patching, and Nuzlocke persistence. Related shared UI and Nuzlocke changes are under `3ds/core/` and `3ds/host/`.
- **Tests:** new host input/state/display/save-tool tests, diagnostic host feature and UI tests, `libretro/test/core_gameplay_test.c`, shared display coverage and CI updates exercise the new paths. The host integration programs are original diagnostics, not commercial ROMs.
- **Other touched surfaces:** `.github/workflows/libretro.yml`, `.github/workflows/linux.yml`, `docs/manual/linux.md`, `linux/README.md`, and Windows CMake compatibility headers are also modified. The Windows changes are source preparation only, not a verified Windows port.

The frontend still loads `pokedaisy_gba_libretro.so` for GBA on armGBA, `pokedaisy_gbc_libretro.so` for GB/GBC on mGBA, and `pokedaisy_ds_libretro.so` for DS on DSperate. The companion, HUD, quick menu, `EXPAND` and `ZOOM` belong to the cores. The host owns input, audio, pacing, battery saves, states, display filters and the game list.

## Fresh Ubuntu setup

The documented native prerequisites are CMake 3.18 or newer, a C11 compiler, C++17 for the DS core, SDL2 development files, zlib development files and Python 3. Git is included here because a fresh laptop needs it for the checkout and submodules.

```sh
sudo apt update
sudo apt install -y git cmake build-essential libsdl2-dev zlib1g-dev python3
cd ~/src/pokedaisy
git submodule update --init third_party/mgba third_party/armgba third_party/zlib
git submodule update --init --recursive third_party/dsperate-lr
git submodule status third_party/mgba third_party/armgba third_party/zlib third_party/dsperate-lr
```

The recursive DS step is required because `third_party/dsperate-lr` contains its own `DSperate` submodule. The other emulator submodules used by Android, melonDS or the older DS experiment are not needed for these three Linux cores. No Android SDK, private decomp checkout or retail ROM is needed for the normal build and synthetic tests.

### Keep armGBA clean for Linux

A fresh Linux submodule is the pinned `third_party/armgba` gitlink with no local patch. The 3DS configuration is deliberately different: it may apply `third_party/patches/armgba-pokedaisy.patch` to the submodule worktree. That patch is for 3DS, not the Linux libretro core. The initially inspected submodule contained exactly that tracked patch, so it did not represent a fresh CI-like Linux core until the integration maintainer reversed it.

The superproject sets `ignore = dirty` for this submodule, so a clean-looking superproject status is not enough. Compare the recorded gitlink with the submodule revision and inspect the submodule itself:

```sh
git rev-parse HEAD:third_party/armgba
git -C third_party/armgba rev-parse HEAD
git -C third_party/armgba status --short
git -C third_party/armgba diff --exit-code
```

On a clean Linux checkout the two revisions match, status prints nothing and the diff check succeeds. If the 3DS patch is present, the following commands preserve the old diff and reverse exactly that tracked patch. Run the second command only when the reverse check succeeds:

```sh
git -C third_party/armgba diff --binary > ~/armgba-before-linux.patch
git -C third_party/armgba apply --check --reverse \
  ../patches/armgba-pokedaisy.patch && \
git -C third_party/armgba apply --reverse \
  ../patches/armgba-pokedaisy.patch
git -C third_party/armgba status --short
git -C third_party/armgba diff --exit-code
```

This does not reset or clean the submodule and does not discard unrelated work. If the reverse check fails, stop and inspect the diff. If status or the final diff is nonempty after reversal, other changes remain and the tree is not yet a clean CI-like baseline. Do not force them away.

Use a fresh GBA build directory after reversing a previously patched tree, and use separate worktrees when 3DS and Linux configurations are both needed. A later 3DS configure may apply the patch again. Before every claimed clean Linux result, repeat the inner status and diff checks.

## Build GBC, GBA, DS and the host

Run all commands from `~/src/pokedaisy`. Keep each core and the host on the same source revision. The examples use `-j2` to match the documented low-resource build; increase it if the new laptop has capacity.

### GB and GBC core, plus the synthetic core test

```sh
cmake -S libretro -B build/gbc -DCMAKE_BUILD_TYPE=Release
cmake --build build/gbc --target pokedaisy_gbc_libretro core_gameplay_test -j2
```

### GBA core

```sh
cmake -S libretro/armgba -B build/gba -DCMAKE_BUILD_TYPE=Release
cmake --build build/gba --target pokedaisy_gba_libretro -j2
```

### DS core

```sh
cmake -S linux/libretro-ds -B build/ds -DCMAKE_BUILD_TYPE=Release
cmake --build build/ds --target pokedaisy_ds_libretro -j2
```

### Stage the cores and build the SDL2 host

```sh
mkdir -p build/cores
cp build/gbc/pokedaisy_gbc_libretro.so \
   build/gba/pokedaisy_gba_libretro.so \
   build/ds/pokedaisy_ds_libretro.so \
   build/cores/
cmake -S linux -B build/host \
  -DCMAKE_BUILD_TYPE=Release \
  -DPOCKET_TEST_CORES="$PWD/build/cores"
cmake --build build/host -j2
```

`POCKET_TEST_CORES` must be set at configure time to register the DS diagnostic and the four core-backed host integration tests. If the host was configured before the cores existed, rerun the configure command.

Run the development binary without installing it:

```sh
mkdir -p ~/PokeDaisy/roms
POCKET_CORE_DIR="$PWD/build/cores" \
  build/host/pokedaisy-linux --data ~/PokeDaisy
```

With no ROM argument, the game list reads `~/PokeDaisy/roms`. A direct launch can instead append `/path/to/your/game.gba`. Use only your own dumps and keep them outside the repository.

## Core and shared-code tests

These are the Linux-relevant core gates. They do not run the Android full suites.

### Synthetic libretro gameplay test and exported symbols

```sh
ctest --test-dir build/gbc --output-on-failure -L synthetic
sh libretro/test/check_exports.sh build/gbc/pokedaisy_gbc_libretro.so
sh libretro/test/check_exports.sh build/gba/pokedaisy_gba_libretro.so
sh libretro/test/check_exports.sh build/ds/pokedaisy_ds_libretro.so
```

`core-gameplay` needs no ROM. It covers Center actions, party rollback, GBA battle input, delayed trade-evolution requests, queued Center scripts and atomic Nuzlocke replacement. There is no separate CTest suite registered by `libretro/armgba`; its core is checked by the export test and the GBA diagnostic host flows.

### DS core tests

Build the test targets as well as the core, then run every registered DS test:

```sh
cmake --build build/ds -j2
ctest --test-dir build/ds --output-on-failure
```

This includes layout policy, the enhanced libretro contract in both pixel formats, shared default UI, settings, companion fixtures and parity, and the battle strip.

### Shared portable-core tests

```sh
make -C 3ds test-host-display test-dex test-stats
```

These use the repository's public fixtures. They are the shared display, dex and stats checks wired into the libretro workflow, not the complete 3DS or Android test suites.

## Complete Release host test run

With all three staged cores and the host configured as above:

```sh
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
```

The expected 14 entries are:

```text
linux-io-test
linux-wild-test
linux-input-test
linux-state-test
linux-display-shader-test
linux-save-tools-test
package-smoke
portmaster-launcher
diagnostic-roms
diagnostic-nds
linux-host-test
linux-host-features
linux-save-ui-test
linux-display-host-test
```

The last four run the real host headlessly against the staged cores. CTest supplies the dummy SDL video/audio drivers and software renderer. The diagnostic GB, GBA and DS programs are generated by the fixture tests and contain no commercial game data.

To rerun only the four core-backed integration entries and their fixtures:

```sh
ctest --test-dir build/host --output-on-failure \
  -R '^(linux-host-test|linux-host-features|linux-save-ui-test|linux-display-host-test)$'
```

`linux-wild-test` has optional ROM-backed coverage. `POKEDAISY_ROM_DIR` can name a directory containing the specific retail filenames listed in `linux/test_wild.c`. With the variable unset, it passes after checking only the blank-image guard and reports `0 ROM(s)`. Do not mistake that result for retail WILD ENCOUNTERS verification.

```sh
POKEDAISY_ROM_DIR=/path/to/your/rom-directory \
  ctest --test-dir build/host --output-on-failure -R '^linux-wild-test$'
```

## Optional retail-ROM core coverage

Retail checks are separate from the required synthetic suite. Supply only your own dumps at configure time. Every option is optional, and an omitted cart is skipped. No path below is assumed to exist on either laptop.

```sh
cmake -S libretro -B build/retail -DCMAKE_BUILD_TYPE=Release \
  -DPD_FIRERED_ROM=/path/to/firered-rev1.gba \
  -DPD_EMERALD_ROM=/path/to/emerald.gba \
  -DPD_UNBOUND_ROM=/path/to/unbound.gba
cmake --build build/retail --target core_gameplay_test -j2
ctest --test-dir build/retail --output-on-failure -L retail
```

FireRed checks pinned Center addresses, Emerald checks Center addresses and reusable-TM/instant-text discovery, and Unbound checks that a rewritten nickname special is not offered. The optional joypad-route procedure for summary actions is documented in [../libretro/test/README.md](../libretro/test/README.md). It uses retail media and public RAM fixtures, so it is not part of the synthetic gate.

Keep retail files and their paths out of commits, issue attachments and package directories.

## Package and install on the new laptop

Create a new package output directory. `linux/package.py` deliberately refuses to overwrite an earlier package tree.

```sh
cd ~/src/pokedaisy
python3 linux/package.py build/host/pokedaisy-linux build/dist \
  --core build/cores/pokedaisy_gba_libretro.so \
  --core build/cores/pokedaisy_gbc_libretro.so \
  --core build/cores/pokedaisy_ds_libretro.so
python3 -m zipfile -l build/dist/pokedaisy-linux-x86_64.zip
```

The archive contains the executable, all three cores, launcher metadata and license notices. It must not contain ROMs, saves or states. `linux/package.py` writes `SOURCE.txt` from the superproject's `HEAD` and submodule gitlinks only. It does not capture dirty superproject changes or patches inside a submodule worktree, and its historical `Fork` URL is stale. Before the integration commit, the patch and untracked archives remain necessary provenance. After that commit, with clean pinned submodules, the committed hash is the authoritative source identity. Keep later WIP archives with any dirty-build package and do not rely on the generated `Fork` field.

Install into a separate application directory and keep player data outside it:

```sh
mkdir -p ~/Applications/PokeDaisy-wip ~/PokeDaisy/roms
cp -a build/dist/pokedaisy-linux-x86_64/. ~/Applications/PokeDaisy-wip/
cd ~/Applications/PokeDaisy-wip
./pokedaisy-linux --data ~/PokeDaisy
```

The three `.so` files stay beside `pokedaisy-linux`, so `POCKET_CORE_DIR` is unnecessary in the installed layout. On a runtime-only Ubuntu machine, install `libsdl2-2.0-0` rather than the development packages. For a desktop shortcut, use an absolute `--data` argument. Without `--data`, the host uses its current working directory.

For a boot smoke check with a retail ROM and a disposable data directory:

```sh
mkdir -p ~/PokeDaisy-smoke
~/Applications/PokeDaisy-wip/pokedaisy-linux \
  --data ~/PokeDaisy-smoke /path/to/your/game.gba
```

A boot screenshot or a fixed `--frames` run is only a smoke check. It is not a substitute for playing, saving, rebooting and reloading.

## Application, data and backup paths

These paths use the example installed and data directories above.

| Path | Contents |
| --- | --- |
| `~/Applications/PokeDaisy-wip/pokedaisy-linux` | Installed host executable |
| `~/Applications/PokeDaisy-wip/pokedaisy_*_libretro.so` | The three cores loaded from beside the executable |
| `~/src/pokedaisy/build/` | Disposable core, host, test and package build trees |
| `~/PokeDaisy/roms/` | Your uncompressed `.gb`, `.gbc`, `.gba` and `.nds` files |
| `~/PokeDaisy/saves/` | Raw battery saves, normally named after the whole ROM filename plus `.sav` |
| `~/PokeDaisy/saves/pokedaisy-backups/` | Newest ten distinct pre-open battery backups |
| `~/PokeDaisy/backups/` | Pre-open backups when `--save FILE` overrides the normal save path |
| `~/PokeDaisy/states/` | State slots, thumbnails, undo data, selected-slot markers and automatic-resume bundles |
| `~/PokeDaisy/saves/pokedaisy/settings.ini` | GB/GBA companion and shared presentation settings |
| `~/PokeDaisy/saves/pokedaisy-ds/settings.ini` | DS companion settings |
| `~/PokeDaisy/host.ini` | Keyboard/controller mappings, hotkeys, display and host settings |
| `~/PokeDaisy/library.txt` | Favorites, recents, view, sort and grid/list state |
| `~/PokeDaisy/library-titles.txt` | Rebuildable ROM title cache |
| `~/PokeDaisy/covers/` | Downloaded or cached cover art |
| `~/PokeDaisy/gamecontrollerdb.txt` | Optional additional SDL controller mappings |
| `~/PokeDaisy/save-tools-cache.txt` | Rebuildable save-tool library metadata |

For `game.gba`, the normal battery save is `saves/game.gba.sav`. If that does not exist, `saves/game.sav` is a compatibility fallback, unless another ROM with the same stem makes it ambiguous. Slot 1 is `states/game.gba.armgba.ss` on armGBA. GB/GBC and DS use `states/game.gb.ss` or `states/game.nds.ss`. Slots 2 through 10 insert `.slotN` before `.ss`. Keep each state's `.thumb`, `.bak` and `.bak.thumb` sidecars with it. State formats belong to the core that wrote them.

Automatic resume adds `.resume.ss`, `.resume.sav`, `.resume.ready`, `.fresh` and related sidecars under `states/`. Copying the whole directory preserves these relationships. A state is never a replacement for a verified battery save.

### What to migrate

Close PokeDaisy before copying anything. Keep an independent backup of the old data, then copy these private files to the corresponding locations on the new laptop:

1. Your own ROMs from `~/PokeDaisy/roms/`, or point `POCKET_ROM_DIRS` at their existing folders.
2. The entire `~/PokeDaisy/saves/` tree, including `pokedaisy-backups/`, `pokedaisy/` and `pokedaisy-ds/`. This carries battery saves, backups, companion settings, flight-area data and Nuzlocke records.
3. The entire `~/PokeDaisy/states/` tree if you need states, undo, selected slots or automatic resume. Test them with the same core and build before relying on them.
4. `host.ini`, `library.txt` and any custom `gamecontrollerdb.txt`.
5. `covers/` if you want to keep downloaded art. `library-titles.txt` and `save-tools-cache.txt` can be copied or regenerated.

Do **not** transfer `steamgriddb-key.txt` in a source archive or commit it. If you use SteamGridDB, recreate that file privately on the new laptop. Never migrate data by placing it inside `~/src/pokedaisy`.

On first launch, confirm that the game loads its battery save before loading any state. Check the pre-open backup appears, make a new in-game save, exit, reopen and verify it persists. Keep the old data untouched until this succeeds.

## Controller mapping status and physical validation

The code and synthetic tests now cover SDL buttons and trigger axes, remapping conflicts, clearing bindings, keyboard compatibility and persistent settings. Controller hotplug handling is implemented but **untested**. New automated hotplug cases are required, as noted in the upstream contribution plan, and they will not replace physical validation. SDL GameController mappings come from SDL's own database, `gamecontrollerdb.txt` in the data directory or `SDL_GAMECONTROLLERCONFIG`, and take priority over the raw fallback. The first mapped controller is used. `POCKET_RAW_TRIMUI=1` is only the explicit TrimUI fallback, not a general fix for an unknown desktop controller.

The physical Manba controller was disconnected during this round. Its SDL mapping, analog trigger thresholds, trigger release behavior and hotplug behavior are **unverified**. Synthetic `POCKET_PAD` events do not prove that its hardware or driver reports the same controls.

Use a disposable data directory so validation does not overwrite your normal mappings:

```sh
mkdir -p ~/PokeDaisy-controller-test/roms
POCKET_WINDOW=1280x720 \
  ~/Applications/PokeDaisy-wip/pokedaisy-linux \
  --data ~/PokeDaisy-controller-test
```

Copy one of your own test ROMs into that directory's `roms/`, then validate as follows:

1. Start from a terminal and check the log for the controller name and GUID. If SDL reports it unmapped, supply a community-format mapping in `~/PokeDaisy-controller-test/gamecontrollerdb.txt` or through `SDL_GAMECONTROLLERCONFIG`. Do not enable the raw TrimUI layout for the Manba.
2. Open a game, then open the host menu with START+SELECT, Guide/Menu or Escape. In `CONTROLLER`, check every game and host action. Confirm D-pad and left-stick directions, face buttons, START, SELECT, L1/R1, L2/R2, L3/R3 and any paddles the controller exposes. The right face button is game A and the bottom button is game B by default.
3. Bind both analog triggers. Check press, partial travel, full travel and release for each, and make sure no direction or action sticks. Test conflict moves, B to cancel and SELECT to clear. Restart the program and confirm `host.ini` preserves the bindings.
4. Test SELECT+R2 for fast-forward and, after enabling `REWIND`, SELECT+L2 for rewind. A plain SELECT tap must still reach the game on release. Bind and test save/load, undo save/load and previous/next slot. Test the separate DS hotkey set as well.
5. In GB/GBA, check X for the companion and L3 for the quick menu. In DS, check L2 for the companion, R2 for screen switching, L3 or held L2 for the quick menu, SELECT+Y for stylus mode, and SELECT+D-pad/A for stylus movement/taps. Also test behavior with the host modifier off.
6. Unplug and reconnect the controller during play. Verify recovery without restarting, then check `A/B BUTTONS` in both `NORMAL` and `SWAPPED` if the physical layout needs it.

Record the Manba's GUID, mapping source, firmware/driver, USB or Bluetooth connection, and the result of each trigger and hotplug check. Only after that should the physical mapping limitation be closed.

## Verification evidence at handoff

These are recorded results from the existing worktree. They were not rerun for this documentation-only handoff.

- **Release host:** the standard Release configuration with all three cores passed **14/14** CTest entries. The saved run includes controller remapping, ten slots, thumbnails, undo, resume, save import, save editor/trade UI and actual display-pixel checks.
- **Clean armGBA reproduction:** the integration maintainer reversed exactly the tracked 3DS-only `armgba-pokedaisy.patch`, rebuilt the GBA core and reran the complete host suite successfully at **14/14**. Emerald also booted with this clean, CI-like core. This establishes the integration baseline, not the result of future extracted slices.
- **Core tests:** synthetic `core-gameplay` passed with `core gameplay tests passed`. An Emerald-only optional retail run also passed its Center scan and reusable-TM/instant-text scan; the FireRed and Unbound cases were skipped.
- **Sanitizers:** the functional tests passed in their normal suite, but the sanitizer run was **not a passing suite**. It had leak-related failures, including `linux-save-ui-test` and `linux-display-host-test`. No invalid memory accesses were reported in the exercised ASan/UBSan paths, but leak ownership remains unresolved. Do not describe the sanitizer run as passed or leak detection as clean.
- **Retail boot:** a retail Emerald ROM booted in the local host, in addition to the later clean-core boot above. These are boot smoke results only, not full live gameplay or save-cycle results.
- **ROM-free limitation:** the recorded `linux-wild-test` pass reported zero retail ROMs checked. The host integration evidence otherwise comes from generated diagnostics and public fixtures.

For sanitizer follow-up, use a separate build tree so the Release objects stay untouched:

```sh
cmake -S linux -B build/sanitize \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPOCKET_TEST_CORES="$PWD/build/cores" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/sanitize -j2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ctest --test-dir build/sanitize --output-on-failure
```

That command instruments the host, while the staged cores remain the Release cores. Rebuild a core with matching sanitizer flags when investigating ownership inside that core. Treat each leak report as unfinished work, not as an expected warning to suppress.

## Remaining limitations and next checks

- **Physical controller and hotplug:** Manba mapping and both triggers still need the hardware procedure above. Hotplug is implemented but has no automated coverage and also needs physical validation.
- **Windows:** Windows was not configured, built or tested. The modified compatibility files do not establish a working Windows binary.
- **Gameplay depth:** most automated evidence is synthetic or diagnostic. Emerald boots, including the clean-core boot, and ROM table scans do not verify a complete play session, field writes, Center/evolution flows, battery persistence, state compatibility or DS gameplay on real hardware.
- **Sanitizer ownership:** the sanitizer run had leak-related failures, including `linux-save-ui-test` and `linux-display-host-test`. Reproduce them, analyze ownership and obtain a clean sanitizer rerun before claiming sanitizer validation.
- **Save-tool variant coverage:** admission uses header/layout checks rather than a retail SHA allowlist. The synthetic suite does not establish every advertised retail family or European language, so those variants need focused evidence or narrower claims.
- **Android parity:** the Linux port does not have complete Android feature or game-support parity. Do not assume every Android companion tab, field operation, DS feature, hack/language port or setting is present. Check the Linux manual and the relevant core before claiming support.
- **Upstream preparation:** the integration branch is not one ready pull request. The new libretro gameplay behaviors still need their per-slice player-manual updates and per-ROM review, as detailed in [docs/UPSTREAM_CONTRIBUTING.md](UPSTREAM_CONTRIBUTING.md).
- **Source provenance:** use the fork branch's committed `HEAD` for the integrated source. The historical base commit alone is not enough. For later dirty builds, `HEAD` and submodule gitlinks do not capture the WIP, so preserve new patch and untracked-file archives and verify that armGBA is clean. `linux/package.py` also retains a stale historical `Fork` URL, as noted in the contribution plan.

The next useful validation order is: resolve or isolate the leaks, validate the Manba triggers, run real battery-save and reboot cycles on all three cores, exercise the new gameplay flows with retail media, then assess Windows and Android-parity gaps separately.

## Safe update and rebuild workflow

1. **Protect the work and data.** Close the app, back up `~/PokeDaisy`, refresh the WIP patch and untracked archive, and inspect `git status --short`. Never use a clean or reset operation to make an update easier.
2. **Check what will be shared.** Use `git diff --name-only` and `git ls-files --others --exclude-standard`. Keep ROMs, saves, states, `local.properties`, `local.mk`, API keys, controller logs with private paths and build output out of commits and packages.
3. **Integrate deliberately.** Once local work is safely committed or otherwise recoverable, fetch both remotes and inspect the changes before merging:

   ```sh
   git fetch origin
   git fetch upstream
   git log --oneline --decorate HEAD..upstream/main
   git diff --stat HEAD..upstream/main
   git merge upstream/main
   ```

   Resolve conflicts without discarding the Linux modules. Once the branch is published, use fast-forward-only pulls from its fork branch. Do not force-push over another developer's work.
4. **Refresh only the required submodules.** Run the two submodule update commands from the setup section after changing revisions. Inspect `git submodule status`, then run the separate inner-armGBA revision, status and diff checks. Superproject status alone can hide the 3DS patch. If needed, reverse exactly that patch with the checked procedure above.
5. **Rebuild as one revision.** Rerun the GBC, GBA, DS and host commands above, replace all three files in `build/cores` together, and reconfigure the host with `POCKET_TEST_CORES`. For toolchain, CMake-option or armGBA-patch changes, use fresh build directory names instead of mixing old objects. Do not mix cores from different source revisions or clean/patched armGBA configurations in one package.
6. **Run the focused gates.** Run synthetic core tests, DS CTest, shared `test-host-display test-dex test-stats`, export checks and the full 14-entry Release host suite. Add optional retail coverage only with your own ROMs. Follow the repository rule to ask before full Android or whole-project suites.
7. **Smoke the packaged result.** Use a new package output directory, inspect the zip, install into a separate directory and start with a disposable data folder. Only then point it at migrated real data. Keep the previous application and data backups until battery saves, states and controls are confirmed.
8. **Update documentation with behavior changes.** Player-visible Linux changes belong in `docs/manual/linux.md` in the same change. If `docs/manual/linux-install.md` changes, regenerate its install guides with `node website/scripts/install-guides.mjs`. The website and manual checks require Node 22.23.4 or newer and the locked website dependencies. After installing Node with nvm or a local Node distribution, run:

   ```sh
   cd ~/src/pokedaisy/website
   nvm use
   npm ci
   cd ..
   make docs-check
   ```

   `make docs-check` runs the manual and search tests, then builds the website. The integration handoff passed all 9 manual/search tests and the Astro build with Node 22.23.5. Follow [docs/UPSTREAM_CONTRIBUTING.md](UPSTREAM_CONTRIBUTING.md) for upstream preparation and review.
