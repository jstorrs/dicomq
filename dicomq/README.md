# dicomq — operator guide

A qmail-inspired DICOM store-and-forward suite. [DESIGN.md](DESIGN.md)
is the architecture contract; this file is how to run it.

## Create a spool

dicomq never creates directories — creating them *is* configuration:

```sh
SPOOL=/var/spool/dicomq
mkdir -p $SPOOL/{queue/{tmp,todo},route,aet,dest,failed,hold,corrupt}
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
maildir ./ env
forward PACS1
EOF
```

## Define a forwarding destination

```sh
mkdir -p $SPOOL/dest/PACS1 $SPOOL/route/PACS1/todo
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

## Operate

```sh
dicomq-queue                       # what is queued where, ages, backoff
dicomq-queue PACS1                 # per-message detail for a destination
dicomq-super hold|release|requeue|fail <id>
touch $SPOOL/route/PACS1/hold      # freeze a destination; rm to thaw
```

`failed/` is the bounce pile — point your alerting at it. `corrupt/`
holds quarantined malformed messages. dicomq never deletes from either;
inspect, then `dicomq-super requeue` or `rm`.

## Test

```sh
bash test/run-tests.sh <bindir>    # or: ctest --test-dir <builddir>
```

The integration suite drives the real binaries against a throwaway
spool, including network legs against DCMTK's storescu/storescp.
