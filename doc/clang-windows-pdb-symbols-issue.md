# clang→mingw lld PDB: C++ symbols don't resolve — SOLVED

## TL;DR
Two real bugs plus one measurement artifact:

1. **No debug info at all.** The engine compiled with `-gcodeview` but no `-g`. `-gcodeview`
   only selects the *format*; without a `-g*` level clang emits zero CodeView, so the PDB
   had section-contribs + publics but no per-module symbol/line streams → every address
   fell back to the nearest public (`_sm_mbucket`, …) with `[unknown]` lines.
2. **Full `-g` doesn't link here.** `-g` + the mandatory `-femulated-tls` makes clang emit a
   `.debug$S` record for every `thread_local` that references the *bare* TLS symbol, which
   emulated-TLS never defines → `ld.lld: undefined symbol: cur_record` on any image with a
   thread_local (Backend.cpp, smmalloc, libstdc++ std::call_once …).
3. **llvm-symbolizer on Linux is a bad proxy.** The exe also carries **DWARF** (from the
   gcc-mingw CRT/libs); llvm-symbolizer prefers that partial DWARF and falls back to
   PDB-publics-only for everything else, so C++ looked broken even after the PDB was fixed.
   dbghelp/Tracy on Windows read the **PDB** and ignore DWARF — they resolve fine.

**Fix (one line):** `CMakeLists.txt` uses `-gline-tables-only -gcodeview` (was `-gcodeview`).
That emits S_GPROC32 + line tables + S_INLINESITE (everything Tracy callstacks need) while
omitting the variable-level records that break the emulated-TLS link. Names come from the
COFF-derived publics already in the image.

**Verified:** after a clean build, C++ frames resolve to name + file:line, e.g.
`0x140323ba0 → CGlobalRendering::GetDisplayBounds(SDL_Rect&, int const*) const @
rts/Rendering/GlobalRendering.cpp:1409`. (On Linux you must strip the exe's DWARF first to
see it — see below; on Windows/Tracy no stripping is needed.)

## Why `-gline-tables-only`, not `-g`
`-femulated-tls` is mandatory (toolchain): gcc-mingw libstdc++ emits emulated-TLS symbols and
clang defaults to native TLS, so without it std::call_once etc. fail to link. But emulated-TLS
+ full CodeView is broken: the `.debug$S` for a `thread_local` carries SECREL/SECTION relocs
to the bare symbol (`_ZL10cur_record`), and emulated-TLS only defines `__emutls_v.`/`__emutls_t.`
versions → undefined at link. `-gline-tables-only` omits those variable records (no loss for
callstack symbolization) and links clean. Proof:
```
clang++-19 --target=x86_64-w64-mingw32 -femulated-tls -gcodeview          -c tls.cpp # .debug$S=0 (no info)
clang++-19 ...                          -femulated-tls -g -gcodeview        -c tls.cpp # links? NO: undefined _ZL10cur_record
clang++-19 ...                          -femulated-tls -gline-tables-only -gcodeview -c # links: OK, GPROC+lines+inlines
```

## The DWARF-shadowing artifact (don't get fooled again)
`spring.exe` contains both a PDB (external) **and** DWARF sections (`.debug_info`,
`.debug_line`, … from the gcc-mingw CRT/static libs). `llvm-symbolizer` finds the DWARF first,
uses it for the CRT, and for everything else degrades to PDB *publics nearest-match* — so all
60 probes in the original investigation collapsed onto two `extern "C"` names. The PDB was
actually correct. To symbolize C++ on Linux, strip the exe's DWARF so the symbolizer commits
to the PDB:
```
cp build-winclang/spring.exe /tmp/s.exe && llvm-objcopy-19 --strip-debug /tmp/s.exe
llvm-symbolizer-19 --obj=/tmp/s.exe 0x140323ba0   # the embedded PDB path still points at spring.pdb
# -> CGlobalRendering::GetDisplayBounds(...) @ GlobalRendering.cpp:1409
```
dbghelp (Tracy, Windows) only reads the PDB, so this strip is a Linux-only diagnostic step.
(Optional: the DWARF is dead weight in the Windows exe; can be dropped at link to shrink it.)

## Things that were RULED OUT
- **lld version / PDB-writer bug** (doc's old hypothesis 4): relinking the same objects with
  llvm-mingw's **lld 22.1.7** (vs system **lld 18.1.3** — note `/usr/bin/ld.lld` is 18, there is
  no `ld.lld-19`) produced a byte-identical PDB and identical symbolizer output. Not lld.
- **Section contributions / COMDAT mismap** (hypotheses 1 & 3): the contribs map the C++ VMA to
  the correct module (147 = GlobalRendering.cpp) and the GPROC32 addr matches exactly. Fine.
- **publics address-map incomplete**: the records are complete and well-distributed (41,471
  publics, 39,968 distinct addresses, none at 0). Fine.

## Repro / verify
```
cmake --build build-winclang -j 32            # CMakeLists already has -gline-tables-only -gcodeview
cmake --install build-winclang                # PDBs are NOT installed by default; copy them:
for p in spring spring-headless spring-dedicated unitsync; do
  cp build-winclang/$p.pdb build-winclang/install/ 2>/dev/null; done
# Linux sanity check (strip DWARF first, see above). On Windows just point Tracy at the PDB.
```
Do NOT pass `-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O3 -DNDEBUG"` (it drops the default `-g`; the
explicit `-gline-tables-only` in CMakeLists now makes the debug level independent of that).

## Inspect commands (reference)
```
llvm-symbolizer-19 --obj=<exe-with-dwarf-stripped> <VMA>
llvm-pdbutil-19 dump -modules|-publics|-symbols -modi=<N>|-section-contribs|-section-headers|-l <pdb>
llvm-readobj-19 --sections <obj|exe>            # check for .debug$S (CodeView) / .debug_info (DWARF)
```
