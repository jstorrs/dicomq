# storescp+

`storescp+` is [DCMTK](https://dcmtk.org/)'s `storescp` (a DICOM C-STORE
Storage Service Class Provider) with a Maildir/qmail-inspired delivery mode.
It builds as a standalone project against an installed DCMTK.

## The `--imagedir` delivery mode

With `--imagedir`, received objects are delivered the way qmail delivers to a
Maildir — written somewhere temporary, then atomically renamed into place —
so a consumer watching the output directory never sees a partial file:

1. Each incoming object is written to `<output-dir>/tmp/` under a unique name:

   ```
   tmp/[CalledAET].[CallingAET].[YYYYMMDDHHMMSSMMM].[PID].[COUNTER].[MODALITY].dcm
   ```

2. After the object has been received and written completely, it is
   `rename(2)`d to its final destination:

   ```
   [CalledAET]/[CallingAET].[YYYYMMDDHHMMSSMMM].[PID].[COUNTER].[MODALITY].dcm
   ```

   The destination directory is named after the Called AE Title if a writable
   directory of that name exists under the output directory; otherwise
   `new/` is used. This lets one receiver fan deliveries out to per-AET
   directories simply by creating them.

`storescp+` never creates directories: `<output-dir>/tmp/` and
`<output-dir>/new/` must exist before it starts, as must any per-AET
destination directories you want to receive into.

AE titles are sanitized before use in paths: trimmed, and anything other
than alphanumerics and `-` replaced by `_` (an AET that sanitizes to the
empty string becomes `_`). Because `.` is always replaced, the
dot-separated filename fields are unambiguous. A Called AET that
sanitizes to `tmp` (in any case) is never used as a destination
directory; such deliveries fall back to `new/`. Filenames are unique
across concurrent receivers by construction (timestamp + PID + per-process
counter).

`--imagedir` conflicts with the stock filename/sorting/event options that it
replaces (`--timenames`, `--sort-*`, `--exec-on-*`, `--rename-on-eostudy`,
`--eostudy-timeout`, `--exec-sync`).

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
