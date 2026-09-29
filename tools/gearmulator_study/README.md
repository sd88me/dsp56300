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
3. **Next concrete step:** identify what memory address the poll instruction at $0001db actually reads
   (disassemble/check the discovery trace at that PC), then trace writes to that address backward from DSP
   instruction #13,179,646 to find the first build-dependent write -- almost certainly a genuine
   miscompilation of one specific opcode whose wrong output only becomes observable this far into execution.
   Block-granularity and whole-loop coalescing are already ruled out for this (MAX_INSTR=1 + no-loop
   still reproduced it in the previous session), so don't re-test those.
3. Once bit-exact (or a second real bug is found and fixed), re-measure Force timing (last measured: 4.75x
   real time, worse than the interpreter's own eventual target of 100%; a proper Stage-3-style optimisation
   pass, never done for this synth, is likely needed before it's usable in the port).
- Ask before restarting MPC or touching `MPC.settings`, per the `mpc-vst-plugin` skill.
