# Contributing the Linux desktop parity work upstream

This plan splits the integrated work on `linux-desktop-parity` in `SneezeGUI/pokedaisy` into focused pull requests for
`lidor30/pokedaisy`. The integration work starts at `fc5048dda9d3c1b5947e176ad8a6e8a39b2d0d8e` and is committed as `3a310f5d`.
Do not propose the whole integration branch as one pull request.

Start each slice from the current upstream default branch. Copy the relevant hunks, not whole copies of coupled files such as
`linux/main.c`, `libretro/pd_libretro.c`, `libretro/pd_lr_gameplay.c`, or `docs/manual/linux.md`. Inspect the integration commit and
its added files with `git show 3a310f5d` rather than relying on an empty working-tree diff. After a slice merges, rebase the
integration branch and remove the corresponding hunks so later slices contain only new work.

This is a plan only. It does not open pull requests or substitute integration-level evidence for focused tests on each slice.

## Rules for every upstream pull request

### Public content and commit metadata

- Never include ROMs, battery saves, save states, Nuzlocke runs, secrets, signing material, API keys, or data extracted from a
  player's save. Use the original diagnostic programs and synthetic saves in the repository for required tests. Optional local
  ROM tests must use the tester's own dumps outside the repository, for example under `~/roms/`.
- Do not bundle game art. Keep the existing rule that art is rebuilt from the player's own ROM.
- Write home paths as `~` in every committed file, including test instructions and fixture documentation. Do not copy a real
  username, machine-specific checkout path, local SDK path, or runner path into a pull request.
- Commit authorship and messages must carry no AI attribution. This includes Claude or Anthropic attribution, `Co-Authored-By`
  trailers, and `Generated with` lines. Do not bypass the local commit message hook. Keep generated attribution out of committed
  documentation as well.
- Review the final staged file list and diff for test output, core dumps, generated binaries, states, saves, ROMs, local settings,
  and accidental secrets before every submission.

### Documentation and player-facing features

- Update `docs/manual/` in the same pull request for every player-visible feature, setting, default, control, supported game, or
  behavior change. `linux/README.md` and `libretro/test/README.md` are developer documentation, not substitutes for the manual.
- Follow `docs/manual/README.md`: plain second-person writing, exact labels in backticks, no em or en dashes, no spaced hyphen
  used as punctuation, and no real Pokémon, move, or item names in manual prose or screenshots. Game titles are allowed.
- Update setting keywords and `docs/manual/synonyms.yml` or page keywords when players need new search terms. Render and inspect
  every changed UI screen. Manual screenshots come from the documented renderers, never from a player's ROM or save.
- A page that has the eight documented translations needs all eight updated. Regenerate the GitHub install guides with
  `node website/scripts/install-guides.mjs` when one of those install pages changes. Check the resulting generated files rather
  than editing them independently.
- Every new Android player-facing feature needs its `NewFeatures.<ID>` in `NewFeatures.ALL` and
  `labelBadge = NewFeatures.badge(...)` on the relevant settings rows on both screens. The badge lasts seven days from the first
  launch of a build containing it. The portable C UI does not use this Compose API, so do not add a meaningless Android badge for
  a Linux, libretro, or 3DS-only row. If such a row needs a NEW marker, use that frontend's existing mechanism or raise the design
  question in the pull request.
- The current manual diff covers host controls, states, display filters, and save tools. It does not document the new libretro
  battle, party, bag, box, Center, Nuzlocke, or runtime ROM option behavior. Those slices need their own manual updates before
  they are ready upstream.

### Per-ROM review and generated data

- Review `docs/ROM_FEATURES.md` for every gameplay slice. A change to a tracked capability for a ROM, or a new supported ROM,
  requires its row and any relevant note in the same commit. Check retail revisions, language ports, in-place hacks, CFRU and
  expansion layouts, and negative cases. A signature match on one retail build is not evidence for every row.
- Runtime discovery must fail closed on missing, duplicate, ambiguous, or changed code. Do not turn a partial or negative mark
  into a positive one from a synthetic test alone. Record the actual headless, device, save, and reboot coverage in the pull request.
- Keep per-game data in `gamedata/`. New Gen 3 configs and changed addresses belong in TOML, not generated Kotlin or C. A new
  field also belongs in `gamedata/schema/gen3_config.toml`. Run `make gamedata` and the focused generator and migration checks
  when this changes. Do not hand-edit generated `NativeConfigsGen.kt`, `GameDataRecordsGen.kt`, `3ds/core/pd_configs_gen.h`, or
  generated DS port files.
- AREA map rows come from `scripts/gen_area_maps.py --scan <ROM folder>`, never from hand editing. Wide tables, flight records,
  language ports, and DS data have their own generators and `--check` or migration gates. Run the gate for any generator touched
  by a slice.
- The inspected integration diff does not change `gamedata/` or the generated wide and flight tables. Do not add generated-file
  churn to these slices unless the source data actually changes.
- Player-facing portable-core labels also require `python3 3ds/tools/gen_tr.py`, a reviewed `3ds/core/pd_tr_gen.c` diff, and
  `make -C 3ds test-tr`. A feature branch is not ready while the generated translation check reports that `pd_tr_gen.c` is stale.

### Tests and platform claims

- Run focused tests while preparing a slice. In the app, prefer `make test-quick T='*RelevantTest'`. For the native work, build
  and run only the new synthetic or diagnostic targets for that slice. Full local suites, including `make test`, `make check`,
  `make test-roms`, unfiltered Gradle unit tests, and full preview renders, need the user's approval first.
- Full CI must pass on every upstream pull request before merge. A focused local pass does not replace CI, and a skipped optional
  ROM test is not a pass. Split the monolithic integration tests by feature so an early pull request does not need later features
  to compile or pass.
- Keep required CI tests ROM-free. Retail ROM and physical-device checks can be separate, explicitly named local validation. Do
  not put local ROM paths or test saves in CI configuration.
- Validate Linux and its libretro cores against a clean `third_party/armgba` submodule at the superproject's pinned gitlink. The
  Linux armGBA build does not apply `third_party/patches/armgba-pokedaisy.patch`. The 3DS configure is deliberately different:
  `3ds/emu/armgba.cmake` includes `apply_armgba_patch.cmake`, which applies that tracked patch. Do not present a Linux result from
  a 3DS-patched submodule as a clean-core result. Check the submodule revision and diff before building, using separate worktrees
  when both configurations are needed.
- Parent verification for this handoff: with armGBA clean at the pinned gitlink, the Linux build passed all 14 host tests and
  booted Emerald. This establishes the integration baseline, not the result of any future extracted slice. Repeat the relevant
  checks on each pull request and require upstream CI.
- Windows must be built and tested before claiming that a slice is Windows compatible. A MinGW cross-compile alone does not
  validate filesystem replacement, file permissions, controller input, SDL text entry, rendering, or save rollback. Keep Windows
  source lists current with each slice, and perform native Windows smoke tests for the affected behavior before making a runtime
  compatibility claim.
- No Manba mapping was found in the inspected diff. Do not upstream a Manba mapping until it has been validated on the physical
  device and controller. Check its SDL identity, every mapped button and trigger, menu and game behavior, hotplugging, and saved
  bindings. Injected SDL events are not physical validation.

## Recommended pull request order

The first two numbered slices are deliberately small fixes, with an independent package-provenance candidate beside them. Slices
3 to 5 build the host foundations, slices 6 to 11 add core gameplay, and the remaining slices add display and save tooling.
Dependencies are part of the order, not suggestions to combine the pull requests.

### 1. Fix party-swap rollback after read or write failures

**Scope:** Extract `pd_party_swap_records` from `3ds/core/pd_party.c` and `3ds/core/pd_party.h`, then use it in
`3ds/host/pd_session.c`. The old failure path could write uninitialized stack buffers when an initial read failed. Restore only
records that were actually read, and preserve rollback after a partial swap or verification failure. Do not include the libretro
party feature in this pull request.

**Tests:** Extract only the party-swap synthetic cases from `libretro/test/core_gameplay_test.c` into a small test target that
needs no Center or gameplay adapter. Cover success, rejected indices, both initial read failures, each partial write failure, and
verification failure. Strengthen the current failed-read case: its final assertion checks only `memoryImage[0]`, while the swap
targets start at offsets 10 and 110. Assert that both complete target ranges remain unchanged.

**Depends on:** Nothing. This is a good first upstream candidate.

### 2. Preserve the old Nuzlocke run when replacement fails

**Scope:** Add `pd_nuz_save_atomic` in `3ds/core/pd_nuzlocke.c` and its declaration, then use it from `3ds/host/pd_session.c`.
The old session code removed the previous run before `rename`, so a failed rename could lose it. Write a temporary file, close it
successfully, and use the platform's atomic replacement without deleting the destination first. Keep the old run and the new
temporary file on failure for diagnosis and recovery. Leave libretro Nuzlocke tracking for slice 10.

**Tests:** Use an injected rename failure to prove that the old text remains and the new temporary file survives, then prove that
a successful replacement removes the temporary name. Use synthetic text in a temporary directory, never a real run.

**Depends on:** Nothing. This can be proposed independently of slice 1.

### Early candidate: Record package provenance without losing upstream attribution

**Scope:** Update `linux/package.py` so `SOURCE.txt` describes the actual build rather than its packaging history. Its historical
`Fork: https://github.com/keeganwhitacre/pokedaisy-brick` URL is stale and must not remain as an asserted source. Preserve the
canonical `Source: https://github.com/lidor30/pokedaisy` line and the required PokeDaisy attribution in NOTICE, documentation, and
credits. Add the public remote used for this build, with an explicit CI override and an honest unknown fallback, the build commit,
an explicit dirty-tree marker, and the expected submodule gitlinks. Distinguish clean, dirty, and intentionally patched submodule
states instead of implying that a patched tree is the pinned clean core.

Use only a sanitized public remote. Strip credentials and tokens, never emit a local checkout path, and do not replace a missing
or unreadable remote with another historical guess. Document the resulting fields in `linux/README.md`.

**Tests:** Extend `linux/test_package.py` and the package smoke coverage with temporary source repositories. Cover clean and dirty
trees, untracked files, a fork remote, a missing remote, the CI override, canonical upstream attribution, submodule revisions,
removal of the stale Fork claim, and rejection or redaction of credentialed and local-path remotes. Check both normal and
PortMaster archives without writing secrets or real home paths into test fixtures.

**Depends on:** Nothing. This is a small independent candidate alongside slices 1 and 2, not a prerequisite for the feature work.

### 3. Add unified keyboard and SDL controller remapping

**Scope:** Extract `linux/host_input.c` and `linux/host_input.h`, the `CONTROLLER` page, SDL button and trigger capture, conflict
moves, clear and cancel behavior, persistent `pad_*` settings, hotplug and input-reset changes, and the widened input bit masks.
Keep existing keyboard names and modifier hotkeys working. Add keyboard L3 and R3 here, but leave undo and slot actions for slice
4. Keep the left stick as a D-pad alias and retain the raw TrimUI fallback's fixed layout. Update the relevant CMake source lists
and the manual's control sections.

**Tests:** Run `linux-input-test` and extract only the existing controller, keyboard compatibility, and pointer-cancel cases from
`test_host_features.py`. Hotplug cases do not currently exist there, so add coverage for controller arrival, removal, mapped-device
selection, and stale-input reset. Cover all actions, conflicts, clearing, invalid stored values, capture cancellation, and no stale
held input after remapping. Use injected SDL device and button events for automated coverage, then perform physical-controller
hotplug validation separately.

**Depends on:** Nothing beyond the upstream base.

### 4. Add atomic host files, ten state slots, thumbnails, and undo

**Scope:** Extract `linux/host_state.c` and `linux/host_state.h`, unique temporary files, synced atomic publication, callback-owned
frame copies, ten slot paths with slot 1 keeping the legacy name, selected-slot persistence, thumbnail sidecars, one-level save
undo, and load undo with a committed way back. Include the battery backup and baseline changes needed by state loading. Add the
new state hotkeys to the controller foundation from slice 3. Keep core-specific state formats and core validation intact.

Do not include automatic resume or save import yet. The frame-copy helper is shared with display filters later, so make its
ownership and pixel-format behavior explicit rather than duplicating it in slice 12.

**Tests:** Run `linux-state-test` and the state-only portions of the diagnostic host integration test. Cover legacy paths, slots
2 and 10, empty slots, thumbnails, save and load undo, backup failure before overwrite, interrupted writes, invalid thumbnails,
and no leftover temporary files. Exercise both the GB and armGBA diagnostic cores. Test old core states only against the formats
the core actually accepts.

**Depends on:** Slice 3 for the new input actions and menus.

### 5. Add safe automatic resume and battery-save import

**Scope:** Add optional `AUTO RESUME`, its state and battery bundle, content-hashed commit manifest, fresh marker, stale-battery
rejection, and clean-exit write. Add `--import-save` and `--restore-save`, size and RTC-layout validation, the normal pre-open
backup, atomic replacement, and stale resume invalidation. Include the core restart path for a failed automatic deserialization
so a partially touched core is not exposed to the player.

**Tests:** Extract the resume and import integration cases. Cover clean resume, changed battery, missing or mixed bundle parts,
corrupt manifest, state write failure, battery write failure, ready-manifest failure, failed deserialize followed by a clean
restart, valid import, invalid size, and resume suppression after import. Verify that failed writes never replace the previous
known-good state or battery save.

**Depends on:** Slice 4. Save-tool UI integration in slice 14 relies on the same fresh-marker contract.

### 6. Add bounded libretro writes and the battle-input adapter

**Scope:** Introduce `libretro/pd_lr_mem.h`, bounded RAM and ROM writes for mGBA and armGBA, read-back checks, and armGBA JIT
invalidation after a ROM write. Add the minimal `pd_lr_gameplay` lifecycle needed to initialize, reset on state load, poll, and
run jobs. Move the existing Polished Crystal input ownership into that adapter without changing its behavior, then enable GBA
battle actions through `pd_binput` only when discovery succeeds. Include the required armGBA stubs and both cores' build rules.
Do not enable party, bag, box, Center, Nuzlocke, or ROM-option rows in this slice.

**Tests:** Extract the synthetic battle-action cases, including unavailable functions, menu timing, cancellation, and state jumps.
Use the real `pd_lr_host` joypad route for local diagnostic coverage. Review the existing battle-button and one-press rows in
`docs/ROM_FEATURES.md`; this slice exposes an existing per-ROM capability through another frontend rather than defining a new ROM
capability.

**Depends on:** No host-state or display slice. It can proceed in parallel with slices 3 to 5 after review capacity is available.

### 7. Add party and bag actions with safe deferred jobs

**Scope:** Add party swapping through the helper from slice 1, item use, give and take, TM teaching, trade-evolution requests,
personality and party-count rechecks, and the summary controller-focus fixes. Execute game functions only at safe points between
frames. Gate every row through validated field, party, TM, and trade-evolution functions. Include the small signed party-count
comparison correction in `3ds/host/pd_items.c` here as a type-safety cleanup, not as a claimed user-visible bug fix without a
reproducer.

**Tests:** Extract delayed-evolution, item-target, party-change, and summary-navigation cases. Run the focused shared host display
test and real joypad-route checks where a diagnostic fixture can reach the UI. Add negative tests for eggs, battles, locked field
state, changed personality, failed restore after an evolution probe, and unsupported layouts. Use local ROM-backed checks for each
ROM family claimed in the manual.

**Depends on:** Slices 1 and 6.

### 8. Add box reading and party/box moves

**Scope:** Enable `pd_boxes` reads and the withdraw, deposit, and swap jobs, including box selection, party-full behavior, message
handling, and post-action rereads. Keep the existing storage-layout and function validators. Do not infer that a hack supports
compressed, plaintext, or relocated box records merely because its base retail row supports box moves.

**Tests:** Cover successful operations, full party, full box, last-fighter and mail restrictions where applicable, changed
personality, failed reads or writes, and unsupported storage layouts. Check the resulting live state through an in-game save and
reboot for each newly claimed layout. Keep those saves local and out of the pull request.

**Depends on:** Slice 7 for the shared item, party, and deferred-job adapter.

### 9. Add validated Pokémon Center services

**Scope:** Add `3ds/core/pd_center.c` and `3ds/core/pd_center.h`, Center discovery, summary overlays, heal, nickname, forget, and
relearn actions, and queued script completion. Preserve original-trainer, egg, battle, locked-field, last-move, and required-field
move guards. Relearn uses the TM and deferred-job foundation, not the reusable-TM ROM patch from slice 11.

**Tests:** Extract Center action and queued-script synthetic tests. Cover discovery ambiguity, changed specials, party changes,
script timeouts, callback cleanup, and every refusal reason. The optional test currently names FireRed rev 1 and Emerald discovery,
plus an Unbound nickname-special rejection. That is not sufficient evidence for LeafGreen, Ruby, Sapphire, every language and
revision, or hacks. Add focused local validation for each claimed row, including an in-game save and reboot for mutating actions,
or narrow the capability and documentation.

**Depends on:** Slice 7.

### 10. Add libretro Nuzlocke tracking and persistence

**Scope:** Wire the shared tracker into snapshots, battle and field transitions, settings, manual outcome edits, warnings, and per
ROM CRC persistence. Use the atomic save helper from slice 2. Keep unsupported games, including the explicitly excluded Rogue
family, from exposing the feature. Reset transient tracking safely on a state jump without discarding the persisted run.

**Tests:** Cover encounter and faint transitions, trainer battles, duplicate rules, warnings, manual edits, new runs, reload, and
state jumps. Keep atomic replacement coverage from slice 2, and add adapter tests for enable, disable, unsupported games, and
reload from a synthetic run file.

**Depends on:** Slices 2 and 6. It does not need Center or box moves.

### 11. Add reusable-TM and instant-text runtime options

**Scope:** Extract ROM-option discovery, expected-byte validation, idempotent apply and restore, read-back verification, and
state-jump resynchronization. Show built-in reusable-TM behavior as built in rather than as a patch. Keep trade evolution in slice
7 and TM teaching in slice 7's bag work; this pull request is only for runtime ROM patches.

**Tests:** Cover every discovery site, too many or duplicate sites, expected bytes, already patched bytes, changed code, partial
write failure, toggling on and off, and state reload. The current optional retail scan covers FireRed and Emerald option
discovery, not every retail port or hack. Update `docs/ROM_FEATURES.md` only where this changes a tracked per-ROM claim, and
validate those exact ROMs before changing a mark.

**Depends on:** Slice 6 for ROM writes and JIT invalidation. Slice 7 should land first if the same adapter exposes TM teaching,
so unsupported intermediate UI states are avoided.

### 12. Add bounded CPU display filters

**Scope:** Add `linux/display_shader.c` and `linux/display_shader.h`, `DISPLAY FILTER`, configuration migration from the old LCD
setting, LCD compatibility, scanlines, CRT, GBA color, edge smoothing, and SDL final scaling. Reuse slice 4's callback-owned frame
copy. Preserve the unchanged default OFF path, input ownership, row order, alpha, and the bounded 2x intermediate buffers. Do not
read SDL's write-only texture storage.

**Tests:** Run `linux-display-shader-test` and `linux-display-host-test`. Keep the pixel-reference, padded-stride, resize, invalid
argument, overflow, determinism, input-ownership, and single-color-transform cases. Inspect actual diagnostic output for every
mode at fractional and whole scales, including GBA color composition and OFF. Use the optional benchmark locally and check memory
use on the handheld targets rather than treating a desktop benchmark as universal.

**Depends on:** Slice 4 for frame ownership. Native Windows rendering must be exercised before claiming Windows filter support.

### 13. Add the save-tool filesystem adapter

**Scope:** Extract `linux/save_tools.c` and `linux/save_tools.h` without embedding the library UI. Include ROM and save naming,
ROM size limits, read snapshots, stale-file rejection, verified backups, atomic one-save and two-save writes, rollback, permission
preservation, and the portable `pd_library_host` callbacks. Document that no emulator may have either save open during a write.

**Tests:** Run `linux-save-tools-test` with synthetic ROM headers and saves. Cover text callback cancellation, short and missing
reads, stale saves, duplicate destinations, backup failure, failure of each replacement, rollback, read-back verification, legacy
save names, permissions, and repeated adapters in one process. The inspected adapter test constructs FireRed, Emerald, and
LeafGreen layouts. That test alone does not prove every Ruby, Sapphire, European, or other layout advertised by the current manual
paragraph, so add the existing portable-layout evidence or narrow the claim.

**Depends on:** No UI slice. It can move earlier if reviewers prefer to establish save-write safety before host state work.

### 14. Embed save editing and trading in the Linux library

**Scope:** Add `linux/save_tools_ui.c` and `linux/save_tools_ui.h`, library pause and resume, emulator exclusion, supported-game
filtering, modal text entry, unsaved-change confirmation, pointer and controller navigation, session destruction, ROM-header
revalidation, and resume invalidation before either save is replaced. Keep Game Boy, Game Boy Color, DS, Japanese, and unknown or
expanded layouts rejected unless each has explicit support.

**Tests:** Run `linux-save-ui-test` using `linux-save-fixture` and diagnostic ROMs. Cover editor and trade entry, all navigation
routes, text entry and cancellation, stale-save injection, two-save failure, unsaved close through every exit path, fresh markers
for both core suffixes, cache failure, and clean session teardown. Confirm that returning to the normal library cannot launch a
core while a tool session is still alive.

**Depends on:** Slices 3, 4, 5, and 13.

### 15. Finish CI, generated install docs, and platform compatibility

**Scope:** This is a closeout slice, not a place to defer feature documentation or tests. Each earlier pull request should carry
its relevant workflow change: `core_gameplay_test` and shared display tests with libretro, and the matching host integration test
with Linux. Use this final pull request only for remaining cross-cutting CI coverage, Windows portability fixes such as
`pd_win_compat.h`, Windows source-list reconciliation, and generated install-guide updates that were actually needed.

Pin workflow actions to commit SHAs. Keep x86_64 and AArch64 builds, glibc ceilings, export checks, synthetic tests, and original
diagnostic ROM flows visible as separate jobs or steps. Add a Linux CI guard that the armGBA submodule HEAD equals the
superproject's `HEAD:third_party/armgba` gitlink and that its working diff is empty before the clean-core build. Keep
`apply_armgba_patch.cmake` in the 3DS configure path, not in Linux, and never hide an unexpected submodule edit merely to make the
guard pass. There is no Windows workflow in the inspected base. If one is added, distinguish a build-only job from native Windows
runtime validation and do not describe the former as proof of compatibility.

**Tests:** Require the complete upstream CI matrix before merge. When host or core integration changes, repeat the integration build's
clean-gitlink baseline of all 14 host tests and an Emerald boot, recording the verified submodule revision and clean diff. Build
the Windows target and run native smoke tests for input, state replacement and undo, resume invalidation, every display mode, save
editing, two-save trade rollback, and text entry. Keep any unverified platform or device explicitly unverified in the pull request
and manual.

**Depends on:** All slices whose CI or Windows behavior it includes.

## Why `docs/ROM_FEATURES.md` stays unchanged in this handoff

The matrix records which ROM has capabilities that need per-ROM addresses. It is not an Android, libretro, Linux, 3DS, or Windows
parity matrix. The inspected branch adds host and frontend plumbing, controller and state features, display processing, save tools,
and portable adapters over existing game data. It adds no ROM configuration, address table, or newly identified ROM.

Relevant retail rows already mark battle buttons, one-press picks, POKéMON switch, field items, FLY, PC anywhere, reusable TMs,
and TMs from the bag where those capabilities are known. Center services, party reordering, box UI, Nuzlocke tracking, state slots,
controller mapping, display filters, and save tools do not have matrix columns. Changing marks merely because another frontend
now exposes a feature would make the matrix less accurate, not more complete.

Leave the file unchanged for this handoff. During each upstream gameplay pull request, review it again. Update a row or note in
that same pull request if discovery or validation shows that the branch really adds, removes, or limits a tracked capability for a
specific ROM. If upstream wants platform-specific support tracking, propose a separate document or an explicit matrix redesign
rather than overloading the existing marks.

## Final review checklist for every slice

- The pull request contains one coherent behavior and its focused tests, manual updates, generated sources, and CI coverage.
- No unsupported menu row or host capability bit is exposed as a compile-time placeholder for a later slice.
- ROM-backed behavior has a per-ROM support statement that distinguishes synthetic, headless, device, save, and reboot evidence.
- `git diff --check` is clean, generated-file checks are current, and the staged paths contain no local artifacts or real home
  paths.
- Focused tests pass locally, full upstream CI passes, and any unrun optional check is reported as not run.
- Windows compatibility is claimed only after a Windows build and relevant native tests. Manba mapping remains out until physical
  validation.
