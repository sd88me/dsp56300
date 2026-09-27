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
2. The remaining divergence is DSP-side-innocent (cycle/instruction counters match exactly at the point of
   divergence; ruled out: spin-skip, whole-loop recompilation, block-granularity -- see ARM32_JIT.md for how
   each was tested and rejected). It's most likely a 68k microcontroller-side timing difference. **Instrument
   the uC side** (its own PC and cycle count) alongside the existing DSP `GM_REGTRACE` record and re-run the
   same differential-comparator method that found the first two bugs -- don't re-guess DSP-side mechanisms,
   they're fairly exhausted.
3. Once bit-exact (or a second real bug is found and fixed), re-measure Force timing (last measured: 4.75x
   real time, worse than the interpreter's own eventual target of 100%; a proper Stage-3-style optimisation
   pass, never done for this synth, is likely needed before it's usable in the port).
- Ask before restarting MPC or touching `MPC.settings`, per the `mpc-vst-plugin` skill.
