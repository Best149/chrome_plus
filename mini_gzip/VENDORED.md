# mini_gzip (vendored)

This directory is **vendored source**, not a git submodule. It is part of the
repository so the build is self-contained and cannot be broken by a third-party
repository disappearing.

## Provenance

| | |
|---|---|
| Upstream | <https://github.com/shuax/mini_gzip> |
| Upstream commit | `4f793ab` (2017-10-30, "fix bug") |
| Original authors | Wojciech A. Koszek (`mini_gzip`), Rich Geldreich (`miniz`) |
| License | BSD 2-clause (`mini_gzip`), public domain / Unlicense (`miniz`) — see [COPYRIGHT.md](COPYRIGHT.md) |

## Why it is vendored

Chrome++ Next previously consumed this as the submodule
`https://github.com/Bush2021/mini_gzip`, pinned at commit `2eee7df5`. That
repository no longer exists (the whole `Bush2021` account is gone), so
`actions/checkout` with `submodules: true` failed and **every CI build broke**.
No surviving fork of that repository contains the pinned commit, so the content
had to be reconstructed from upstream.

Detours is still a submodule
(<https://github.com/microsoft/Detours>) because that repository is alive and
its pinned commit `d644ce94` is reachable from `main`.

## Differences from upstream

Upstream `mini_gzip` is written to be inlined into a C project and cannot be
consumed from the Chrome++ Next CMake target as-is. Three changes were made,
all marked with comments in the sources:

1. **`mini_gzip.h` gained `#include <stddef.h>` / `<stdint.h>`.** Upstream's
   header uses `size_t`, `uint8_t`, `uint16_t` and `uint32_t` without including
   anything that defines them; it only ever worked because the including `.c`
   file included those headers first.
2. **`mini_gzip.h` gained an `extern "C"` guard and the public function
   declarations.** Upstream's header declares no functions at all, and
   `src/pakfile.cc` declares the three it uses inside its own `extern "C"`
   block but includes `mini_gzip.h` from *outside* it — so without the guard
   the declarations and definitions would disagree on linkage. `mini_gzip.c`
   wraps its definitions in a matching `extern "C"` block; that is a no-op
   while the file is compiled as C (see below) and only matters if it is ever
   built as C++.
3. **`gzip_compress` is no longer `static`** and is declared in the header.
   Upstream keeps it file-local, but `src/pakfile.cc` needs it to re-deflate a
   patched `resources.pak` entry.
4. `miniz.h` gained the same `extern "C"` guard. It only sets
   `MINIZ_HEADER_FILE_ONLY` and includes `miniz.c`, so it stays a
   declarations-only view of miniz and `miniz.c` remains a separate translation
   unit — verified: `mini_gzip.lib` exposes `gzip_compress`, `mini_gz_start` and
   `mini_gz_unpack` and no duplicate miniz symbols.
5. `mini_gzip.c` includes `<stdlib.h>` explicitly (it calls `malloc`/`free` but
   got them transitively from `miniz.c`), casts away `const` in a
   C++-compatible way, and carries an `_MSC_VER` `#pragma warning(disable: ...)`
   block for the warnings upstream code raises at `/W3`.

## Building

`CMakeLists.txt` compiles `mini_gzip.c` and `miniz.c` as **C** via
`set_source_files_properties(... LANGUAGE C)` so the upstream sources stay
valid C. The target include directory is this folder, which is what makes
`#include "mini_gzip.h"` and `#include "miniz.h"` resolve.

## Line endings

The files are pinned as `mini_gzip/** -text` in `.gitattributes`. Without it,
the repo-wide `* text=auto` (plus a `core.autocrlf=true` checkout) rewrites
them to LF on commit — which silently turned upstream's CRLF `miniz.c` into a
different blob. With `-text` the stored bytes are exactly:

- `miniz.c`, `COPYRIGHT.md`, `README.md` — byte-identical to upstream.
- `mini_gzip.c`, `mini_gzip.h`, `miniz.h` — upstream bytes plus the edits above.
  These keep upstream's own mixed endings, including the single stray CRLF on
  `mini_gzip.c`'s `free(mem_out);` line inside `mini_gz_unpack`.


