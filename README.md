# fs-updater-service

D-Bus daemon for the F&S Update Framework. Owns the `de.fsembedded.fsupdate1` bus name and acts
as the state owner for cloud-flow update operations.

## D-Bus identity

| Item | Value |
|------|-------|
| Bus name | `de.fsembedded.fsupdate1` |
| Object path | `/de/fsembedded/fsupdate1` |
| Interface | `de.fsembedded.fsupdate1` |

## Build

```bash
./scripts/build.sh debug [--lib <build_dir>]   # cross-compile Debug
./scripts/build.sh release                      # cross-compile Release
./scripts/build.sh test                         # native build + unit tests
./scripts/build.sh clean
```

`--lib <build_dir>` points to a local `fs-updater-lib` build instead of the installed SDK library.

## Runtime files

| File | Purpose |
|------|---------|
| `data/de.fsembedded.fsupdate1.conf` | D-Bus bus policy |
| `data/de.fsembedded.fsupdate1.policy` | polkit actions |
| `data/de.fsembedded.fsupdate1.service` | D-Bus activation unit |
| `data/fs-updater.service` | systemd service unit |
| `data/50-fsupdate.rules` | polkit ADU rule |
| `dbus/de.fsembedded.fsupdate1.xml` | D-Bus introspection XML |
