# dicomq — operator guide

A qmail-inspired DICOM store-and-forward suite. [DESIGN.md](DESIGN.md)
is the architecture contract; this file is how to run it.

## Build

Requires CMake ≥ 3.16 and DCMTK ≥ 3.7.0 with development headers
(`libdcmtk-dev` on Debian/Ubuntu provides the CMake package config).

```sh
cmake -B build
cmake --build build
cmake --install build   # installs the eight dicomq-* binaries
```

If DCMTK is installed in a non-standard prefix, point CMake at it with
`-D CMAKE_PREFIX_PATH=/path/to/dcmtk-prefix`.

## Create a spool

dicomq never creates directories — creating them *is* configuration:

```sh
SPOOL=/var/spool/dicomq
mkdir -p $SPOOL/{queue/{tmp,todo},route,aet,dest,failed,hold}
```

Every program takes `-s <spool>` or honours `$DICOMQ_SPOOL`
(default `/var/spool/dicomq`). The spool must be one filesystem,
not NFS.

## Accept a Called AE Title

```sh
mkdir -p $SPOOL/aet/ARCHIVE/{tmp,new}
```

That's it: associations addressed to `ARCHIVE` are now accepted, and
objects land in `aet/ARCHIVE/new/` (Maildir-style — consumers watch
`new/`, may move or delete files, must never modify them in place).
Optional per-AET files:

```sh
# inbound transfer syntax preference, most preferred first ("*" = all)
cat > $SPOOL/aet/ARCHIVE/accept <<EOF
JPEGLSLossless
ExplicitVRLittleEndian
ImplicitVRLittleEndian
EOF

# routing instructions (default when absent: "maildir ./")
cat > $SPOOL/aet/ARCHIVE/deliver <<EOF
maildir ./
forward PACS1
EOF
```

## Define a forwarding destination

```sh
# todo holds fresh objects; retry/1..N are the retry rungs (see Operate)
mkdir -p $SPOOL/dest/PACS1 $SPOOL/route/PACS1/{todo,retry/{1..8}}
cat > $SPOOL/dest/PACS1/remote <<EOF
host: pacs1.example.org
port: 11112
aet: PACS1
EOF
cat > $SPOOL/dest/PACS1/propose <<EOF
JPEGLSLossless
ExplicitVRLittleEndian
transcode: lossless        # never | lossless | as-needed
EOF
```

## Run

```sh
dicomq-send &                      # the queue runner (or systemd, below)
dicomq-recv --listen 11112         # small sites / testing
```

For production, run one `dicomq-recv` per association under systemd
socket activation — unit files are in [systemd/](systemd/): install
`dicomq-recv.socket`, `dicomq-recv@.service`, `dicomq-send.service`,
and the `dicomq-clean` timer; create the `dicomq` and `dicomq-recv`
users. Local submission and re-queueing use `dicomq-inject -c <aet>
<file.dcm>...`.

## TLS

```sh
mkdir $SPOOL/tls                    # inbound: key.pem + cert.pem,
cp server.key $SPOOL/tls/key.pem    # optional ca.pem to require and
cp server.pem $SPOOL/tls/cert.pem   # verify client certificates
dicomq-recv --listen 11112 --tls

mkdir $SPOOL/dest/PACS1/tls         # outbound: existence enables TLS;
cp ca.pem $SPOOL/dest/PACS1/tls/    # ca.pem verifies the server, and
                                    # key.pem+cert.pem (optional)
                                    # authenticate us
```

Both sides use the DICOM BCP 195 profile.

## Operate

```sh
dicomq-queue                       # what is queued where, ages, backoff
dicomq-queue PACS1                 # per-message detail for a destination
dicomq-ctl hold|release|requeue|fail <id>
touch $SPOOL/route/PACS1/hold      # freeze a destination; rm to thaw
```

A rejected forward climbs a per-destination retry ladder
(`route/<DEST>/todo` → `retry/1` → `retry/2` → …); `ls
$SPOOL/route/PACS1/retry/3` is every object that has failed PACS1 three
times. You size the ladder by creating the rungs — `mkdir -p
$SPOOL/route/PACS1/retry/{1..8}` allows eight tries — and dicomq never
creates one itself, so a rejection at the top existing rung (or with no
`retry/` dir at all) lands the object in `route/<DEST>/failed/` rather than
digging deeper. Reasons are in the log, not the spool — the rung says how
many times, the log says why.

Each forwarding outcome is recorded **under its destination**, so a fan-out
to two PACS that succeeds at one and fails at the other tells you which:

```
route/PACS1/complete/   # delivered OK — auto-expires (dicomq-clean -G, 72h)
route/PACS1/failed/     # gave up after the last retry rung — your bounce pile
route/PACS1/corrupt/    # turned out unreadable when forwarding
```

`route/<DEST>/failed/` is the bounce pile — point your alerting at it.
dicomq never deletes from `failed/` or `corrupt/`; inspect, then `dicomq-ctl
requeue` or `rm`. Only `complete/` is reaped automatically. (The top-level
`failed/` now holds just *pre-routing* failures — an unknown called AET or a
`deliver` line with no usable destination — which have no `<DEST>` to file
under.)

## macOS

Build against Homebrew's DCMTK (`brew install dcmtk`; point CMake at it
with `-D CMAKE_PREFIX_PATH=$(brew --prefix)` if needed). Deployment
units live in [launchd/](launchd/) — `org.dicomq.recv.plist` uses
launchd's inetd-compatibility mode, which hands each connection to a
fresh `dicomq-recv` on fd 0 just like systemd `Accept=yes`.

### Running as a regular user (no root)

dicomq needs no privileges: the spool is ordinary files, the default
port (11112) is unprivileged, and `dicomq-recv --listen` needs no
socket supervisor. The quickest unprivileged setup is two terminal
commands and a spool in your home directory:

```sh
export DICOMQ_SPOOL="$HOME/Library/Application Support/dicomq"
# create the spool directories as in "Create a spool" above, then:
dicomq-send &
dicomq-recv --listen 11112
```

For something that survives logout/login, install the per-user launchd
agents instead:

```sh
sh launchd/user/install.sh
```

This creates the spool, fills in [launchd/user/](launchd/user/) plists
for your account, and loads them from `~/Library/LaunchAgents` — the
same socket-activated, process-per-association setup as the system
deployment, just in your user session. macOS will ask once to allow
incoming connections. What you give up relative to the system
deployment is only the two-user privilege separation (everything runs
as you) and port 104; neither affects the delivery guarantees.

Platform notes:

- Durability uses `F_FULLFSYNC` on macOS (plain `fsync` only reaches
  the drive cache there), falling back to `fsync` on filesystems that
  don't support it.
- APFS is case-insensitive by default: called AETs that differ only in
  case map to the same `aet/` directory and share deliveries. The
  reserved names (`tmp`, `new`, `todo`) are checked case-insensitively
  everywhere, so the queue's own directories are safe regardless.
- `dicomq-send` waits on the periodic scan (no inotify); delivery
  latency is the scan interval (`-i`, default 10s) instead of
  milliseconds.

## Test

```sh
bash test/run-tests.sh <bindir>    # or: ctest --test-dir <builddir>
```

The integration suite drives the real binaries against a throwaway
spool, including network legs against DCMTK's storescu/storescp.

## License

dicomq is licensed under the **GNU General Public License, version 3 or
later** (GPL-3.0-or-later); see [`LICENSE`](LICENSE). Because the edge
programs link DCMTK's TLS support against OpenSSL, the license grant
includes an OpenSSL linking exception (a GPL §7 additional permission).

dicomq builds against [DCMTK](https://dicom.offis.de/dcmtk) (OFFIS e.V.,
BSD-style license) rather than bundling it. DCMTK's copyright notice is
reproduced in [`THIRD-PARTY-NOTICES`](THIRD-PARTY-NOTICES) for compliance
when distributing dicomq in binary form.
