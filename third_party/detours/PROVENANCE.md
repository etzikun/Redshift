# Detours provenance

- Upstream: https://github.com/microsoft/Detours
- Release tag: `v4.0.1`
- Commit: `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`
- License: MIT (`LICENSE.md`)

The files below are the complete upstream-derived subset retained by Redshift.
Their Git blobs are byte-identical to the tagged release. Hashes are SHA-256 of
that canonical upstream/Git content. A Windows checkout may present CRLF working
copies when Git's `core.autocrlf` setting is enabled.

| File | SHA-256 |
| --- | --- |
| `LICENSE.md` | `b301808b732cfaa60df2b4b422d78cd97d2a15058b207e7e33f0535ba5170dd6` |
| `src/creatwth.cpp` | `062533627bfa775246948fad92f9f3c0ef7841cc1a83c15db9178be12f2dbf4f` |
| `src/detours.cpp` | `df415e05f985afe88f4b03ba34d6f5abce47edda11cd66d4f72902b801113eb7` |
| `src/detours.h` | `7b498657d8db4cff2d488b1e65cfeeb5b1305de7f32d23588b1fa86aaa46fcf7` |
| `src/disasm.cpp` | `6c44283ac6987d77d92aca93a6a8e9f0dc1f7caf8a2b310e4b374853e9136236` |
| `src/image.cpp` | `8e7c94b335237565f3dce14b6dfd9dd88876cf10fb55b9c910f12ae9c38bd190` |
| `src/modules.cpp` | `4a3f970eb539a4379996eb17e14bd75902fe4f34d566cfd19682ff19315cbc6f` |
| `src/uimports.cpp` | `2b2d92df4c5412906d7e6830a7c2836d78e368a6bef509be028566cbd921e83b` |

`src/uimports.cpp` is not listed as a separate CMake source. It is included
twice by `src/creatwth.cpp` and compiled there for the 32-bit and 64-bit module
formats, as required by upstream Detours.
