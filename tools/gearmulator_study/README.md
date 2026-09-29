# gearmulator study (Osirus / Vavra / Xenia / Nord Lead 2x on the Force)

Results: `docs/ARM32_JIT.md`, "Gearmulator synths on the Force".

- `gearmulator-glue.patch`: apply to `dsp56300/gearmulator` (main @ 9710c1f) after replacing
  `source/cpu/dsp56300` with this fork. Drops the JIT-only calls (`supportBranchAtLoopEnd`, audio workgroup).
- `gm_probe.cpp` + `CMakeLists.txt`: copy to `source/gmstudy/`, add `add_subdirectory(gmstudy)` to `source/CMakeLists.txt`.
  `gm_probe_{virus,mq,xt,n2x} <romdir> [seconds]` runs a scripted workload, prints the output hash, executed DSP
  instructions per audio second and wall/real-time; `GM_HOT=<n>` lists the hottest DSP addresses.
- Build flags: `-DDSP56K_NO_JIT_RUNTIME -DDSP56K_EXEC_STATS -DDSP56K_INTERP_DEFAULT -DDSP56K_INTERP_CYCLES`.
- ROMs are never committed. Note the user's microQ 2.23 dump was byte-swapped (swap each byte pair).

## Resuming (2026-09-27 state)

**Read `docs/ARM32_JIT.md` from "Vavra lock-step + recompiler attempt" through the end** for the full story;
this is just the pointer to where to pick up. None of the scratchpad build directories, discovery traces, or
`.inl` files from prior sessions survive -- only what's committed here does. Rebuilding them is mechanical
(see "Pipeline" above) but takes real time (discovery + build cycles were the bulk of the wall-clock cost
last session); budget for it.

**Where it stands:** Vavra's recompiler no longer hangs (a real bug, found and fixed: a static-init-order
issue in this tool's own opcode decode -- see ARM32_JIT.md for the exact fix, already applied to
`gm_probe.cpp`). The recompiled + lock-step build now completes, deterministically, faster than the
interpreter, and was heard on the Force. It is **not yet proven bit-exact** against the interpreter: one
real divergence remains, located precisely (instruction-index level, via the `GM_REGTRACE`/`GM_REGTRACE_CAP`
differential comparator -- also already fixed for a tracer blind spot found along the way, see ARM32_JIT.md).

**Next concrete step, in order:**
1. Regenerate the discovery trace and lock-step build exactly as ARM32_JIT.md's pipeline describes (needs the
   user's microQ ROM, `~/roms/Waldorf Micro Q/...`, byte-swapped).
2. **Done (2026-09-30): the uC-timing theory is disproven.** The uC side is instrumented
   (`mc68k::Mc68k::s_traceHook`, gated by `DSP56K_RECOMP_DISCOVERY`) and `GM_REGTRACE_UC=1` interleaves DSP+uC
   events into one trace. A 100M-record run each (`tools/gearmulator_study/diff_uc3.py` -- an mmap/generator
   diff, memory-safe; a naive list-based first attempt crashed the host VM outright, not just the process, so
   don't rewrite it back to that) found: the uC event stream *does* eventually diverge, but only downstream
   (by file position) of a DSP-side divergence at DSP instruction counter 13,179,646, where cycle
   count/sr/a/b all match between builds but PC doesn't ($0001e6 interpreter vs $0001db recompiled, the same
   `kind=2` byte-identical poll instruction found last session). Same code, same registers, different branch
   -> a memory/peripheral value at whatever address $0001db polls must differ between builds. Not scheduling
   drift, not decode, not cycle accounting.
3. **Done (2026-09-30, later): identified the register, ruled out a data-corruption theory, found a
   methodology caveat.** $0001db polls bit 15 (`M_PCE`) of Timer2's TCSR (`$FFFF87`, `M_TCSR2` in
   `timers.h`). Added `GM_DISASM=<hex>` (disassemble a P: memory window around a PC, in `gm_probe.cpp`) and
   `GM_TCSR2LOG` (log every TCSR2 change with PC + instruction counter, in `timers.cpp`) -- both are
   permanent, env-gated, zero-cost-when-unused diagnostics now. Result: the write values and event counts
   for TCSR2 are *identical* between builds (57,923 events each, same sequence) -- not a data-corruption bug.
   But the logged PC for the same logical event differs by a constant offset between builds, which points to
   a **trace methodology caveat**: the recompiled build's trace hook fires once per fused block (avg
   3.1 real instructions/block), stamped with the block's *starting* PC, so matching records by raw
   `dspInstr` counter across builds doesn't guarantee comparing the same real moment. The earlier "PC
   mismatch at matching dspInstr" finding is plausibly this artifact, not proof of one instruction branching
   differently on identical state.
4. **The bug is still real, confirmed independent of tracing:** a plain 20-audio-second render gives
   different hashes (interpreter `df13dfba3901a669` vs recompiled `c793b429768eb629`).
5. **Done (2026-09-30, later still): bisected on audio output, found it (probably).** The two builds first
   differ at output sample #730 (16.5ms into a render, not instruction 13-16M as the earlier
   block-granularity-confounded trace suggested). `execCountAll()` isn't comparable between builds (36.8M vs
   7.0M instructions at the identical point!) -- use `getCycles()` instead, the real shared clock. Added
   `GM_REGTRACE_CYCLESTOP`/`GM_REGTRACE_RINGSIZE` (ring-buffer dump once cycles cross a target, avoids
   needing a 150M+ record trace to skip past boot). Nearest-cycle matching is hazardous in a tight repetitive
   polling loop (aliases different loop phases), but consistently found, at two independent points, `sr`
   differing between builds by exactly `0x8000` = **`SR_LF`, the DO-loop-active flag**. This matters because
   `DSP::execRecompiled()` branches on this exact bit.
6. **Checked the generated `recompLoop` bodies directly:** loop-exit correctly delegates to the shared
   `do_end()` (not a reimplementation), and loop-entry's `sr_set(SR_LF)` happens in the shared `do_exec()`
   before calling `execRecompiledLoop()` -- single-loop handling looks architecturally sound.
7. **Next concrete step:** the leading hypothesis is a *nested* DO loop inside a recompiled loop body's
   fused block not correctly re-entering `do_exec()`'s stack bookkeeping for the inner loop. Check whether
   this ROM has a nested DO loop near cycle ~319.8-320M and whether it's fused into an enclosing
   `recompLoop` block in the `.inl`. If that's wrong, fall back to a direct `SR_LF`-write logger (same
   pattern as `GM_TCSR2LOG`) diffed between builds around that cycle range. Full writeup in
   `docs/ARM32_JIT.md`, "Found it (probably)..." section.
3. Once bit-exact (or a second real bug is found and fixed), re-measure Force timing (last measured: 4.75x
   real time, worse than the interpreter's own eventual target of 100%; a proper Stage-3-style optimisation
   pass, never done for this synth, is likely needed before it's usable in the port).
- Ask before restarting MPC or touching `MPC.settings`, per the `mpc-vst-plugin` skill.
