# storescp+

`storescp+` is [DCMTK](https://dcmtk.org/)'s `storescp` (a DICOM C-STORE
Storage Service Class Provider) with a Maildir/qmail-inspired delivery mode.
It builds as a standalone project against an installed DCMTK.

It is also the transitional home of **dicomq**, the qmail-inspired
store-and-forward suite that will eventually replace it — see
[dicomq/DESIGN.md](dicomq/DESIGN.md) for the architecture. All eight
programs (`dicomq-recv`, `-send`, `-local`, `-remote`, `-clean`,
`-inject`, `-queue`, `-super`) are implemented and covered by the
integration suite in `dicomq/test/run-tests.sh`, including TLS,
transcoding, and inotify-driven routing — see
[dicomq/README.md](dicomq/README.md) for the operator guide.

## The `--imagedir` delivery mode

With `--imagedir`, received objects are delivered the way qmail delivers to a
Maildir — written somewhere temporary, then atomically renamed into place —
so a consumer watching the output directory never sees a partial file:

1. Each incoming object is written to `<output-dir>/tmp/` under a unique name:

   ```
   tmp/[CalledAET].[CallingAET].[YYYYMMDDHHMMSSMMM].[PID].[COUNTER].[MODALITY].dcm
   ```

2. After the object has been received, written completely, and flushed
   to stable storage (`fsync`), it is `rename(2)`d to its final
   destination and the destination directory entry is flushed as well:

   ```
   [CalledAET]/[CallingAET].[YYYYMMDDHHMMSSMMM].[PID].[COUNTER].[MODALITY].dcm
   ```

   The destination directory is named after the Called AE Title if a writable
   directory of that name exists under the output directory; otherwise
   `new/` is used. This lets one receiver fan deliveries out to per-AET
   directories simply by creating them.

`storescp+` never creates directories: `<output-dir>/tmp/` and
`<output-dir>/new/` must exist before it starts, as must any per-AET
destination directories you want to receive into. It verifies at startup
that `tmp/` and `new/` exist, are writable, and live on the same
filesystem (atomic `rename(2)` cannot cross filesystems); per-AET
directories on a different filesystem will cause those deliveries to be
refused at store time.

AE titles are sanitized before use in paths: trimmed, and anything other
than alphanumerics and `-` replaced by `_` (an AET that sanitizes to the
empty string becomes `_`). Because `.` is always replaced, the
dot-separated filename fields are unambiguous. A Called AET that
sanitizes to `tmp` (in any case) is never used as a destination
directory; such deliveries fall back to `new/`. Filenames are unique
across concurrent receivers by construction (timestamp + PID + per-process
counter).

Delivery semantics are qmail's: an object is delivered durably *before*
the success C-STORE response is sent, and an object is never delivered
when the response reports failure (the temporary file is removed, the
sender keeps the object, and may retry). If delivery itself fails — for
example the rename fails because `tmp/` and the destination are on
different filesystems — the response reports failure rather than
acknowledging an object that was not delivered. The result is
at-least-once delivery: a crash between rename and response can deliver
the same object twice, as two distinct files. Consumers that need
exactly-once must deduplicate on SOP Instance UID.

`--imagedir` conflicts with the stock filename/sorting/event options that it
replaces (`--timenames`, `--sort-*`, `--exec-on-*`, `--rename-on-eostudy`,
`--eostudy-timeout`, `--exec-sync`), and with `--ignore` and
`--bit-preserving`, which bypass the delivery path.

All other behavior and options are unchanged from stock `storescp`.

## Building

Requires CMake ≥ 3.16 and DCMTK ≥ 3.6.8 with development headers
(`libdcmtk-dev` on Debian/Ubuntu provides the CMake package config).

```sh
cmake -B build
cmake --build build
cmake --install build   # installs bin/storescp+
```

If DCMTK is installed in a non-standard prefix, point CMake at it with
`-D CMAKE_PREFIX_PATH=/path/to/dcmtk-prefix`.

## Tracking upstream DCMTK

`src/storescp+.cc` is a copy of DCMTK's `dcmnet/apps/storescp.cc` carrying a
small patch (~12 hunks) that wires in the `ImageDirManager` class from
`src/storescp+.h`, where all of the delivery logic lives.

The `upstream` branch holds the pristine upstream file and nothing else.
To move to a newer DCMTK:

```sh
scripts/sync-upstream.sh /path/to/dcmtk-source   # commits to 'upstream'
git merge upstream                               # replays our patch on top
```

To see the full local delta at any time:

```sh
git diff upstream main -- src/storescp+.cc
```

## License

Derived from DCMTK, Copyright OFFIS e.V. — see [COPYRIGHT](COPYRIGHT) for
the BSD-style license terms, which also apply to this project's
modifications.
