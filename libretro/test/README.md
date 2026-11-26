# Libretro core tests

The default `core-gameplay` CTest is entirely synthetic. It covers the portable Center engine, party rollback, GBA battle input, delayed trade-evolution requests, queued Center script execution, and Nuzlocke atomic replacement. No ROM is required. `core_gameplay_test` is built by the normal libretro build and by `.github/workflows/libretro.yml`.

```sh
cmake -S libretro -B build/agent-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/agent-tests --target core_gameplay_test -j4
ctest --test-dir build/agent-tests --output-on-failure -L synthetic
```

## Optional retail-ROM coverage

Retail checks are a separate CTest entry, never part of the required synthetic suite. Supply only your own dumps at configure time; do not add them to the repository.

```sh
cmake -S libretro -B build/agent-retail -DCMAKE_BUILD_TYPE=Release \
  -DPD_FIRERED_ROM=/path/to/firered-rev1.gba \
  -DPD_EMERALD_ROM=/path/to/emerald.gba \
  -DPD_UNBOUND_ROM=/path/to/unbound.gba
cmake --build build/agent-retail --target core_gameplay_test -j4
ctest --test-dir build/agent-retail --output-on-failure -L retail
```

Any option may be omitted; that cart is skipped. FireRed checks its pinned Center addresses, Emerald checks its Center addresses plus reusable-TM and instant-text discovery, and Unbound checks that a rewritten nickname special is not offered. The direct executable accepts the same three ROM paths in that order for local diagnostics.

## Optional retail joypad-route checks

The summary-controller integration check uses a retail ROM and the public RAM fixtures, so it is also separate from the synthetic suite. It uses only `pd_lr_host -p` joypad events, never pointer `-t` events: open PARTY with X and A, press DOWN seven times to move through the party into the action row, then use RIGHT and A to activate CENTER. For SWITCH, use DOWN and A on the second row, then A again on the target. A successful CENTER menu and a `Not now` native-field refusal after the switch target are both evidence of the real libretro input path. EVOLVE remains guarded by the same native free-field requirement and is checked synthetically when no suitable free-field fixture is available.
