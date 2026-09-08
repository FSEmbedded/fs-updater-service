# fs-updater-service

D-Bus daemon for the F&S Update Framework. Owns the `de.fsembedded.fsupdate1` bus name and acts
as the state owner for cloud-flow update operations.

## D-Bus identity

| Item | Value |
|------|-------|
| Bus name | `de.fsembedded.fsupdate1` |
| Object path | `/de/fsembedded/fsupdate1` |
| Interface | `de.fsembedded.fsupdate1` |

## The interface

**`dbus/de.fsembedded.fsupdate1.xml` is the interface reference.** It is the published form of the
interface, not merely an introspection dump: every method, property and signal carries its caller,
its precondition and the error it raises, and the file is kept member-for-member in step with the
vtable in `src/service.cpp`. Read it before writing a client; nothing here restates it.

## Build

```bash
./scripts/build.sh debug [--lib <build_dir>]   # cross-compile Debug
./scripts/build.sh release                      # cross-compile Release
./scripts/build.sh sanitize                     # cross-compile Debug with ASan + UBSan, into build-san/
./scripts/build.sh test                         # native build + unit tests (ctest)
./scripts/build.sh test --sanitize              # the same suites under ASan/UBSan
./scripts/build.sh clean
```

`--lib <build_dir>` points to a local `fs-updater-lib` build instead of the installed SDK library.
Pass the lib build that matches the mode — `build_san/` for `sanitize`, `build/` otherwise: the
staged archive carries its own flags, so a sanitized lib in a plain binary fails to link, and a
plain lib in a sanitized binary leaves the library half of the code uninstrumented.
`build.sh` prints its full target and flag list itself; it has no `--help`, so an unknown option is
what produces the usage text (exit 1). It auto-detects a vendored `vendor/libsystemd-dev` when the
host has no libsystemd of its own.

The cross `sanitize` target and the native `test --sanitize` both route through the
`ENABLE_SANITIZERS` option. Passing sanitizer flags via `CMAKE_CXX_FLAGS` instead does nothing on a
cross build: the SDK toolchain file overwrites that cache entry with `FORCE`.

**Run the suite with `FSUP_REQUIRE_BUS=1`.** Without it a run with no session bus reachable skips
the bus-facing cases and still reports success — the variable turns that into a failure, which is
what the CI sets.

Build options: `BUILD_TESTING`, `ENABLE_SANITIZERS`, `FSUP_WERROR`. Defaults live in
`CMakeLists.txt` rather than here, so they cannot drift out of step with it.

The linter is not part of a build: it lives in the container repository as
`../tools/lint-component.sh` and needs a configured sibling `fs-updater-lib` checkout plus the SDK.

## Runtime files

| File | Purpose |
|------|---------|
| `data/de.fsembedded.fsupdate1.conf` | D-Bus bus policy |
| `data/de.fsembedded.fsupdate1.policy` | polkit actions |
| `data/de.fsembedded.fsupdate1.service` | D-Bus activation unit |
| `data/fs-updater.service` | systemd service unit |
| `data/50-fsupdate.rules` | polkit ADU rule |
| `dbus/de.fsembedded.fsupdate1.xml` | The interface reference — see above |
