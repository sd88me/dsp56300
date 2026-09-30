# A 32-bit ARM JIT backend

**Status: investigating.** This fork exists because upstream dsp56300's JIT only targets x86-64 and
64-bit ARM (`source/dsp56kEmu/jittypes.h`), and the maintainers consider 32-bit unsupportable at
their scale (Virus-class synths, [gearmulator#44](https://github.com/dsp56300/gearmulator/issues/44):
"a 32 bit version is not possible ... would not run in realtime"). That verdict was about ~100M+
DSP instructions/second (Virus). Monomachine emulation (Monomodule) needs ~21M/s for one track,
which may be a different answer -- this fork is where we find out, without touching upstream.

Driving use case: porting [Monomodule](https://github.com/shnolk/monomodule) (via
[schwung-monomodule](https://github.com/legsmechanical/schwung-monomodule)'s engine glue) to an
Akai Force/MPC VST2 plugin (`sd88me/mpc-vst-monomodule`, uses this repo as a submodule). Any working
32-bit ARM JIT here benefits every dsp56300-based product on 32-bit ARM (Force is Cortex-A17,
armv7); it is not Monomodule-specific.

## Why upstream won't take this

- 32-bit is explicitly unsupported (README, gearmulator#44). Adding a whole JIT backend for it is
  out of scope for them.
- The JIT is built on [asmjit](https://github.com/asmjit/asmjit), which has no AArch32 (32-bit ARM)
  emitter. We can't extend asmjit's existing back end; a 32-bit ARM code emitter has to be written
  from scratch, scoped to only what dsp56300 needs (see Stage 2).

Keep our changes additive where possible (new files, minimal edits to shared ones) so pulling in
upstream fixes stays cheap.

## Baseline (2026-09-26, Monomodule engine, interpreter only, Force = Cortex-A17 armv7 @ 1.8GHz)

Per 128-frame block against the 2902us deadline, `mnm-bench` on-device:

| machine | load |
|---|---|
| GND SIN (lightest) | 149% |
| FM+ PAR | 220% |
| SID 6581 | 250% |
| DPRO DENS (heaviest) | 284% |

All miss the deadline; average 209%. x86 interpreter build of the same code: FM+ at 33% -- the
Force is ~6.6x slower per emulated DSP instruction than a desktop x86 core.

Tried and rejected/kept:
- `-marm` instead of Thumb-2: 6% *slower* -- rejected.
- PGO (profile on-device, rebuild): ~14% faster (FM+ 220% -> 189%). Kept as a cheap win regardless
  of the JIT outcome, but nowhere near enough alone.
- Interpreter profile (callgrind): cost is spread across decode/AGU/memory/ALU, no single hotspot.

## Stage 0 (done, 2026-09-26): is the instruction mix small enough to bother?

Instrumented interpreter build (a local patch, not yet in this repo -- see `sd88me/mpc-vst-monomodule`
scratch history), ran upstream's `mnm-golden` fixed script on all 22 machines, histogrammed executed
(ALU-part, move-part) instruction pairs.

- 62 instruction-pair forms = 95% of executed instructions (109 = 99%), out of 176 seen.
- Decomposed: ~16 arithmetic mnemonics (MAC/MPY/MACR/ADD/ASL/DIV dominate) x ~8 move address forms.
- Multiply-accumulate alone is ~1/3 of all execution -- ARM32 has a native 32x32->64 MAC instruction,
  good fit for the accumulator's low 48 bits at least.
- Hot code is 1.3-1.8k program addresses per machine -- compiles fast, fits in cache.
- Parallel (ALU+move in one instruction) = 59% of execution.
- Side finding: interpreter's golden-script output hash matched the JIT's for DPRO DDRW/DENS/VO-6,
  didn't for the FX machines checked (THRU onward) -- narrows stage 1's work.

**Verdict: proceed.** A JIT only needs to special-case a few dozen forms well; everything else can
fall back to the interpreter without dominating the cost, *if* the covered forms are cheap enough
and the fallback rate stays low.

## Stage 1 (done, 2026-09-26): make the interpreter bit-exact against the JIT

Needed regardless of the JIT outcome -- it's both the correctness fallback path and the reference
every later stage is checked against. All work below runs entirely on x86 (built against this fork
via schwung-monomodule's `-DMNM_DSP56300_DIR`, no device needed); the Force is only needed for later
timing.

- [x] Resolved upstream's known interpreter/JIT divergence on out-of-range memory reads
      ([dsp56300#8](https://github.com/dsp56300/dsp56300/issues/8)). Root cause confirmed empirically:
      `Memory::get()`/`dspWrite()` unconditionally bailed out (return 0 / drop the write) for any
      offset >= the area's nominal size, *even when the MMU-backed `MemoryBuffer` is active* -- but
      the MMU buffer already maps the whole $000000-$ffffff DSP address range, aliasing every
      out-of-range address onto one shared scratch region (`memorybuffer.cpp`'s comment describes
      this). The JIT (`jitmem.cpp`) already reads/writes through that aliased scratch memory with no
      bounds check at all when MMU support is present; the interpreter didn't. Fixed in `memory.cpp`:
      skip the bounds bail-out when `hasMmuSupport()`. Verified: before the fix, `mnm-golden` (x86,
      `MNM_DSP_INTERP=1`) spammed thousands of `LOG_ERR_MEM_READ` lines across the script; after, zero.
      In practice the scratch region reads back as 0 either way in our engine so this alone didn't
      change any golden hash, but it's a real, confirmed divergence source worth having fixed (matters
      more once code starts writing through it) and is upstreamable as-is.
- [ ] **Still open, still the actual cause of the 10/22 mismatches**: `mnm-golden` hash-compared JIT
      vs. interpreter (x86, this fork, after the fix above) -- still exactly the same 10 machines
      mismatch as in stage 0's spot check: FM+ STAT/PAR/DYN, GND SIN, SWAVE SAW/PULS, DPRO WAVE,
      REVERB, RINGMOD, PHASER. All 12 others (GND ---/NOIS, SWAVE ENS, SID 6581, DPRO BBOX/DDRW/DENS,
      VO-6, THRU, CHORUS, DYNAMIX, FLANGER) already match.
      Narrowed with a raw-sample diff (`mnm-golden`'s `raw-out-dir` arg) on GND SIN: JIT and
      interpreter audio first diverge at **sample 24 of 264448** (i.e. within the first millisecond,
      not after some later parameter sweep), and by a small amount (0.128 vs 0.131) -- not a gross
      garbage/clipped value. That's more consistent with a rounding/precision difference in early
      oscillator or sine-table-build arithmetic than a dramatic saturation blowup, so the "accumulator
      overflow corrupts the sine table" bug from Schwung's `DspEngine.cpp` comment may be real but is
      probably not (solely) this. `getA<T>()`/`getB<T>()` (`limit_transfer`) and `alu_add`/`alu_sub`'s
      `limit_arithmeticSaturation` calls were checked and look correct on inspection -- next step is a
      per-instruction trace comparison (interpreter vs. JIT, same script, diff first mismatching PC)
      rather than more code reading; this is where stage 1 picks back up.
- [ ] Once traced and fixed, `mnm-golden` must hash-match on all 22 machines.

**Second session's findings (2026-09-26, continued):** narrowed the sine-table bug to an exact,
reproducible test vector, and ruled out two plausible-looking causes by testing them, not just
reading code -- both changes below are harmless/more-correct but confirmed **not** the cause:
- Dumped the built 8192-entry sine table (Y:$14A000) directly (no audio rendering needed -- it's
  built once during engine init) and diffed interpreter vs. JIT. They agree exactly through index
  0x1800 (both = 0x800000, i.e. exactly -1.0, the table's minimum). From index **0x1801 onward, every
  remaining entry differs** (2047 of 8192): JIT continues correctly (0x800002, 0x80000a, 0x800016...
  -- values easing up from -1.0), the interpreter's values flip sign (0x7fffff, 0x7fffec, 0x7fffda...
  -- near +1.0 instead). This is a clean, cheap repro: `mnm-dumptable <os.syx>` (a small new tool,
  not yet committed anywhere durable -- see below) vs. the same with `MNM_DSP_INTERP=1`.
- Ruled out: `limit_arithmeticSaturation`'s bit-check (bits 55/48/47 only, vs. the JIT's full
  sign-extend-and-range-check in `alu_saturateSM`) -- genuinely inconsistent logic between the two,
  fixed in `dsp.h`, but `SR_SM` is never set during this script, so the fix is inert here (kept
  anyway, real bug relative to the JIT's own logic, just not *this* bug).
- Ruled out: `alu_mac` (used by `macsu`, one of this loop's instructions) added the old accumulator's
  raw `.var` instead of its sign-extended value, unlike its siblings (`alu_mpy`, `alu_mpysuuu`,
  `alu_dmac`) which all sign-extend first. Looked like a strong match for a sign-flip bug. Turned out
  to be a **numerical no-op**: since the result is masked back to 56 bits (mod 2^56) either way, adding
  the raw two's-complement bit pattern vs. its sign-extended form gives bit-identical results after
  masking -- reverted, not worth the confusion of keeping a change that provably does nothing.
- Not yet checked: the multiply/accumulate chain's actual arithmetic (this loop is a CORDIC-style
  rotation: `mpyuu` (replace, unsigned*unsigned) -> `dmac su` (double-precision MAC, shifts the old
  accumulator right 24 before adding -- a genuinely different accumulation model worth scrutinizing
  on its own) -> `macsu` (normal MAC) -> `dmac ss` -> `asl a` (parallel with `l:(r1),y`) -> `sub y,a`),
  and specifically how `y` (the 48-bit data register, loaded via the parallel move) gets sign-extended
  to 56 bits for the `sub` (`XYto56`/`signextend48to56` -- read but not yet verified against the JIT's
  equivalent bit-for-bit). `alu_dmac`'s own `>>24` old-value handling is also unverified against the
  JIT's `op_Dmac` line by line, beyond confirming both skip SM saturation identically.
- Tooling built for this (in the schwung-monomodule scratchpad, `libs`-linked against this fork via
  `-DMNM_DSP56300_DIR`, not yet copied anywhere durable): `mnm-dumptable` (dumps the sine table for
  diffing), a `MNM_STEP_TRACE` env-gated per-`exec()` register hash hook added to `DspEngine.cpp`
  (useful for coarse checkpointing by matching `getInstructionCounter()` values between runs, but
  **not reliable inside a JIT-compiled hardware loop**: `maxInstructionsPerBlock` does not apply to
  `isRep`/`isFastInterrupt` blocks (`jitblock.cpp:220`), so a `do`/`rep` loop still executes
  atomically -- confirmed by watching `m_instructions` jump by ~8190*10 in one `exec()` call even with
  the block-size limit set to 1). `Jit::setConfig()`/`maxDoIterations` looked like the fix for that
  (meant to give a JIT block an exit point every N loop iterations) but a first attempt to use it
  (setting `maxDoIterations=1` before `resetKeepCode()`, to re-run the init under single-step config)
  hung or never reached the target address within a 20M-step guard -- not debugged further this
  session; if picked back up, check whether `resetKeepCode()` after `destroyAllBlocks()` breaks the
  init handshake, and whether `Y:$14A000`'s `r0` register actually holds the bridged absolute address
  ($14A000+index) rather than a bare index (this was left unverified and may just be a wrong target
  address in the test, not a real hang).
**Resolved.** Found it by building the from-scratch reproducer this doc's previous revision
recommended: captured the exact register state at the real divergence boundary (via a temporary
debug hook in `do_exec`'s loop, dumping registers once `r0` reached the target table address), then
replayed the loop body's 10 instructions from that exact state, once, in both engines, diffing full
registers after every instruction instead of a hash. The two diverged on the *very first*
instruction (`move l:(r1)+,y`), which pointed straight at the *previous* iteration's `move a,l:(r1)`
(the loop's only other write to that address) rather than anything in the instructions between them.

Root cause: `decode_LLL_read`'s case 4/5 ("A"/"B", used by plain `move a,l:(rN)` /
`move b,l:(rN)`) in the interpreter did a raw `reg.a.var >> 24` / `& 0xffffff` bit split with no
scaling or saturation. The JIT's equivalent (`jitops_decode.cpp`, same case 4/5) runs the full
accumulator through `transferSaturation48` first -- scale(), sign-extend, clamp to the 48-bit signed
range, mask. Those only agree when the accumulator's extension byte is still consistent with bit 47;
any iterative accumulation (this loop's CORDIC-style multiply-accumulate chain) can produce a
borderline value where they aren't, and once that happens, the two engines write *different* raw
patterns into the loop's read-back buffer even though the buffer's *visible* effect stayed masked for
a few more iterations by an unrelated coincidence: the table-write instruction (`move a,y:(r0)+`)
goes through `limit_transfer` (24-bit saturation), which happened to saturate both engines' already-
different values to the identical `$800000` right at the table's minimum, so the divergence was
invisible in the sine table itself until the values moved away from that boundary. Fixed by adding
`DSP::limit_transfer48()` (mirrors `transferSaturation48` bit-for-bit) and using it in
`decode_LLL_read`'s case 4/5, in `dsp.h`/`dsp_decode.inl`.

Verified: `mnm-golden` now hash-matches the JIT on **all 22 machines**, and the sine table dumps
(`mnm-dumptable`, both engines) are byte-identical across all 8192 entries. Stage 1's gate is
cleared -- the interpreter is bit-exact against the JIT for Monomodule's actual firmware, not just
in isolated unit tests.

Debug tools built along the way (schwung-monomodule scratchpad, not committed anywhere durable --
cheap to rebuild from this description if needed again): `mnm-dumptable` (dumps the sine table for
diffing, no audio pipeline needed), `mnm-bodydiff` (replays a captured register snapshot through a
fixed instruction range in both engines, diffing full registers after every instruction -- this is
what actually found the bug, and is the technique to reach for first next time, not instruction-count
tracing, which doesn't survive JIT/interpreter hardware-loop batching, see the ruled-out attempts
above). The `MNM_STEP_TRACE` hook added to `DspEngine.cpp` and the temporary `MNM_DEBUG_R0_HEX` hook
added to `dsp.cpp`'s `do_exec` (used to capture the exact register snapshot fed into `mnm-bodydiff`)
were both removed again after use; not committed.

Going forward, re-run this check (`mnm-golden`, x86, no device needed) after any dsp56300 patch
change -- it's the standing regression gate for the rest of this investigation.

## Stage 2 (in progress, 2026-09-26): kill dispatch overhead only (no real code generation yet)

**Where this stands, for the next session:**

- Chose the calling shape: a compiled block is hand-written Thumb-2 bytes in an mmap'd
  `PROT_EXEC` page, structured as `push {r4,lr}; [load r0=this, r1=op, r2=handlerAddr; blx r2]* ;
  pop {r4,pc}`. Every encoding this needs (`movw`/`movt` 32-bit-immediate-load pair, hi-register
  `mov`, `push {r4,lr}`/`pop {r4,pc}`, `blx`) was derived **empirically** -- assembled a probe with
  `arm-linux-gnueabihf-as`, read the bytes back with `objdump -d`, fit a bit-field formula to
  several different immediates to confirm it, never taken from memory of the ARM ARM. See
  `arm32asm.h` (scratchpad, not yet committed to this repo -- small, worth moving into
  `source/dsp56kEmu/` next session as e.g. `arm32blockemitter.h`).
- Proved the core ABI assumption end-to-end, twice:
  1. On a toy class (x86): a non-virtual member function pointer with no multiple inheritance is
     `{address, 0}` under the Itanium C++ ABI (verified: `sizeof` is 2 pointers, second word is 0,
     and calling the first word as a plain `void(*)(T*, Args...)` with `this` as arg0 works).
  2. On the real thing: `test_emit.cpp` (scratchpad) generates actual Thumb-2 machine code with the
     block shape above and runs it under `qemu-arm` calling two real (non-inlined) C functions with
     baked-in operands -- output matched calling them directly. Then `test_callwrap.cpp`
     (scratchpad) did the same against the *real* dsp56300 handlers: resolved `DSP::op_Inc` via
     `DSP::resolvePermutation()`, confirmed its `TInstructionFunc`'s raw bytes are `{address, 0}`,
     and confirmed `DSP::callInstruction(rawAddress, op)` (the new wrapper, see the dsp.h commit)
     produces identical state to calling the resolved handler directly -- checked on x86 and on a
     real armhf binary under qemu-arm (cross-built via `xbuild/armhf.cmake` against
     `-DMNM_DSP56300_DIR=<this fork>`).
  3. Also reconfirmed, on that same armhf cross-build: `mnm-golden` still hash-matches on all 22
     machines under qemu-arm -- Stage 1's fix holds on a real ARM binary, not just x86.
- Added `DSP::callInstruction(void* rawFunc, TWord op)` and
  `DSP::callParallel(void* rawMove, void* rawAlu, TWord op)` (public, dsp.h) as the bridge: a
  generated block can't easily build a real `TInstructionFunc`
  value (or its address, for `exec_parallel`'s by-const-ref params) without emitting data
  alongside the code, so these wrappers rebuild it from a plain address in ordinary C++ instead.
  Committed and pushed.
- Deliberately **not yet done, and not started**: the actual block compiler (walk P memory from a
  PC using `Opcodes`/`getInstructionTypes` -- same APIs Stage 0's histogram tool already used --
  to find non-parallel vs. parallel instructions and where a basic block ends, mirroring
  `op_ResolveCache`'s *decode* logic in `dsp_ops.inl:547` without its *execution* side effect);
  wiring compiled blocks into `DSP::m_jitEntries`/`execJit()` (or, more likely for a first
  measurement -- see below -- bypassing that entirely); and the actual on-device timing comparison
  against the interpreter, which is the whole point of this stage.
- **Scope decision for finishing this stage, so as not to over-build**: don't try to retrofit this
  into `DSP`'s real lazy `m_jitEntries` dispatch machinery yet (that's tightly coupled to the old
  asmjit-based `Jit` class's own lazy-compile-on-first-call trampoline, which would need real
  surgery to share cleanly). Instead, for *this* measurement: eagerly compile a fixed, known
  region -- Stage 1's already-fully-reverse-engineered sine-table loop body (P:$100091-$10009a,
  10 instructions, see Stage 1's section above for the disassembly) is a good first target, since
  its instructions, operands and correctness are already fully understood -- into one block ahead
  of time, and time N repetitions of calling that block directly vs. N repetitions of
  `dsp.execInterpreter()` over the same 10 instructions, on the Force (`root@192.168.1.44`,
  reachable this session). That answers Stage 2's actual question (is dispatch/decode where the
  time goes) without needing the lazy-compilation/chaining machinery Stage 3+ would need anyway.
- Device confirmed reachable this session (`ssh root@192.168.1.44` -- armv7l).
- Built the compiler: `mnm-arm32block.cpp` (schwung-monomodule scratchpad, not yet committed
  anywhere durable -- move it into this repo's `tools/arm32jit_prototype/` next session) walks
  P:$100091-$10009a (Stage 1's sine-table loop body), uses `Opcodes::getInstructionTypes` +
  `DSP::resolvePermutation` to resolve each instruction exactly like `op_ResolveCache` does (minus
  the execution side effect), and emits the Thumb-2 call sequence via `arm32asm.h`, using
  `DSP::prepareOp`/`callInstruction`/`callParallel`. Confirmed on x86 (decode-only path, since
  Thumb-2 bytes aren't valid x86) that it resolves and decodes the whole loop correctly (mpyuu,
  dmac x2, macsu, the `asl a`/`l:(r1),y` parallel pair, the "sub y,a" pair, the nop, both L-moves).
- **Blocked -- found a real, pre-existing, unrelated bug**: running *any* armhf binary built
  against this fork on the real Force crashes with `SIGSEGV, si_code=SEGV_MAPERR`, PC == fault
  address (`0xf505e7c2` in one run) -- a wild jump, not a bad data access. This is **not** Stage 1
  or Stage 2 code: it reproduces with plain `mnm-golden` (Stage 1's own bit-exactness tool,
  unmodified since it passed), at `-O0`/Debug with no LTO, so it's not an optimizer miscompile
  either. `strace -i` pins it inside `MemoryBuffer`'s constructor (`memorybuffer.cpp`, upstream
  code, not touched by this fork's patches) -- crashes right after the last of a long run of
  `mmap2`+`mlock` pairs (the "map scratch blocks to cover the full $000000-$ffffff range" loop,
  the same code Stage 1 traced through to find the OOB-read alias behaviour). Confirmed this is
  **not** a qemu-user artifact: reproduces identically running the binary natively on the device
  over SSH (qemu-arm was actually flakier/inconsistent in this session for unrelated environment
  reasons -- a loader-path issue that came and went; don't trust qemu-user for this repo without
  `QEMU_LD_PREFIX=/usr/arm-linux-gnueabihf`, and even then treat it as a second check, not ground
  truth -- the device is ground truth).
  - This is confusing against `[[monomodule-force-feasibility]]`'s own numbers, which record real
    interpreter benchmarks (150-285% load) *from the Force*, implying the MMU-backed memory setup
    worked at some point on this exact hardware. Whatever toolchain built *that* binary is not the
    one this session's `mnm-armhf-qemu` Docker image + `xbuild/armhf.cmake` produces (GCC 12,
    `-mcpu=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard`) -- worth finding that original build
    (or its flags/compiler version) before assuming this is a genuine 32-bit correctness bug in
    `memorybuffer.cpp` rather than a toolchain regression. PC==faultAddr (a wild branch) is more
    consistent with stack/return-address corruption than a straightforward pointer-arithmetic bug,
    which nudges toward "toolchain/ABI mismatch" over "logic bug", but that's not confirmed.
  - Next step if resumed: either (a) track down the toolchain that produced the numbers already in
    `[[monomodule-force-feasibility]]` and rebuild with that instead, or (b) get a real backtrace
    (no `gdb` on-device or in this session's containers; `strace -i` gave the faulting PC but not a
    call stack -- installing `gdb` cross tools, or copying the core file off the device to analyse
    with `arm-linux-gnueabihf-gdb` on the host, is the way to actually see the corrupted call chain).
- Bail-out gate (unchanged, not yet reached): if the compiled block, once actually run, isn't at
  least ~1.3x faster than the interpreter for that loop on-device, stop -- but this can't be
  measured until the above is resolved.

- Decode each DSP instruction once per basic block instead of every execution; emit a straight-line
  chain of calls into the *existing* interpreter opcode handlers with operands baked in, using a
  hand-written ARM32 code emitter (~a dozen instruction encodings: `bl`, load-immediate, stack setup).
- This isolates "is dispatch/decode the cost, or is it the arithmetic itself" before writing any
  DSP-specific native code.
- **Bail-out gate:** if this isn't at least ~1.3x faster than the interpreter on-device, dispatch
  isn't where the time goes and a full native-codegen JIT won't pay for itself either -- stop here.

## Stage 3: native code for the hot forms, in stage-0's histogram order

- MAC/MPY/MACR/ADD/ASL/DIV first (covers the bulk per stage 0), then address-register arithmetic
  (modulo addressing included).
- After each batch: `mnm-golden` must still hash-match, then re-bench on the Force. Track load vs.
  forms-converted to see the curve early.
- **Bail-out gate:** if the trend after ~5 forms can't plausibly reach ~50-60% of a core for the
  heaviest machine, stop -- 32-bit ARM's register pressure (56-bit accumulators, ~12 usable GPRs vs.
  aarch64's JIT having far more to work with) may make this a losing architecture regardless of how
  much code gets converted.

## Stage 4: finish

- Loops (`DO`/`REP`), block chaining, lazy condition-code flags, interrupts.
- Then the VST wrapper/host-test/skin/bench pipeline in `sd88me/mpc-vst-monomodule`.

## Known risk going in

32-bit ARM has ~12 usable general-purpose registers against 56-bit-wide accumulators and lots of
DSP state (X/Y accumulators, address registers, loop registers, condition codes). The existing
aarch64 JIT has roughly double the registers to work with. Expect heavier register spilling than the
64-bit backends; stages 2-3 will show whether that's fatal or just a smaller win than aarch64's.
Rough pre-measurement guess: 25-40 cycles/instruction on the Force's A17 (vs. ~10 on aarch64 Move
hardware, per schwung-monomodule's docs/PERF.md), landing Monomodule around 35-65% of one core if
stage-0's 95% coverage holds and the uncovered 5% doesn't dominate via interpreter fallback.

**Update (end of session):** the original `build-armhf/mnm-bench` binary, the one behind the
recorded Force numbers, *still runs* on the device today and gets past MemoryBuffer init. So the
crash comes from this session's build (`mnm-armhf-qemu` image + `build-fork-armhf`), not from the
hardware or from upstream `memorybuffer.cpp`. Next session: diff the two builds' compile/link
flags (`build-armhf/CMakeCache.txt` vs `build-fork-armhf/CMakeCache.txt`, and the image each was
built with) and rebuild Stage 2's tools the old way.

## Stage 2 result (2026-09-26, later session): gate FAILED -- JIT effort stopped

**The "MemoryBuffer crash" was never a toolchain or memory bug.** schwung-monomodule's `DspEngine`
defaults to the JIT path unless `MNM_DSP_INTERP=1` is set; on armv7 there is no JIT, so `exec()`
loads through a NULL JIT table (gdbserver backtrace: `DspEngine::runUntilTx`, `ldr r2,[r2=0,...]`).
The old `mnm-bench` numbers were taken with that variable set. With it, every build works, both
`a750f285` and the branch tip, built with `Release` in the old `debian:bookworm` +
`crossbuild-essential-armhf` image and `tools/arm32jit_prototype/toolchain-diff/armhf.cmake`.
(The core goes to the vendor's `az01-coredump` handler, so use `gdbserver` from the bookworm
`gdbserver:armhf` deb plus `gdb-multiarch` in Docker to debug on-device.)

**New open issue (not Stage 2): the armhf interpreter isn't bit-exact with x86.** On the branch tip,
the x86 interpreter matches the x86 JIT on all 22 machines (Stage 1 holds). The armhf interpreter,
on the Force *and* identically under qemu-arm (so it's deterministic, not the hardware), matches on the
15 synth machines but differs on all 7 effect machines (THRU, REVERB, CHORUS, DYNAMIX, RINGMOD, PHASER,
FLANGER). The earlier "armhf matches on all 22" note above is wrong. It is likely a 32-bit
portability bug (`long`/`size_t` width or a shift) in the interpreter or glue. It would matter for
shipping the interpreter on the Force.

**Block compiler on-device** (`tools/arm32jit_prototype/mnm_arm32block.cpp`, fixed this session:
it used callee-saved r5 as scratch without saving it (now r12), and it was missing the i-cache
flush before executing the buffer):
- Runs, but **register state differs** from the interpreter: after the 10-instruction sine loop body,
  `a0` and `y0` come out swapped (interpreter a0=$c62f03 y0=$ba9a81, block a0=$ba9a81 y0=$c62f03).
  This looks like the parallel move/ALU ordering in `callParallel` (the move must read its source
  before the ALU writes). Not fixed, since the gate below failed first.
- **Timing, Force, 2M iterations x 10 instr, 3 runs: interpreter 60-62 ns/instr, block 53-54
  ns/instr, speedup 1.12-1.17x. The gate is >= 1.3x, so it FAILS.** The block also skips the
  interpreter's per-instruction interrupt and loop bookkeeping, so a complete version would be slower
  still. Removing dispatch and decode saves only ~12%, so the time is in the opcode handlers
  themselves (the arithmetic and state access), which is what Stage 3 would have to rewrite natively
  anyway. Per this plan's bail-out rule, **the arm32 JIT effort stops here.**

## Interpreter profile on the Force (2026-09-26): flat, no cheap win. Port shelved.

`perf record -F 2000` of `mnm-bench os.syx 3 3` (all 22 machines, 3 s each, pinned to cpu 3,
`MNM_DSP_INTERP=1`, Release, branch tip). The average load was 241.9% of one core. perf came from
Debian bookworm's `linux-perf:armhf` and its dependencies, unpacked into /tmp and run with
`LD_LIBRARY_PATH`; the Force has an `armv7_cortex_a12` PMU but ships no perf.

| share | where |
|---|---|
| 10.5% | `DSP::op_Parallel` (parallel move/ALU dispatch) |
| 8.7% | `DSP::do_exec` (per-instruction fetch/dispatch) |
| 7.9% | `DSP::alu_mpy` |
| 6.6% | `dspExecPeripherals<Peripherals56303>` (run every instruction) |
| 6.3% | `DSP::op_Mac_S1S2` |
| 4.9% | `DspEngine::runUntilTx` (glue: polls HI08 tx and the instruction budget every instruction) |
| ~3% each or less | AGU update, ddddd decode, the Movex/Movey/Movel/Movexy handlers, Asl, Mpy, Add, `Memory::get`, ... (a long tail) |

Reading it:
- There's no dominant helper. The biggest single symbol is 10.5%, and the MAC/MPY arithmetic totals
  about 18%. Hand-optimizing the arithmetic, even perfectly, is worth at most about 1.2x.
- Per-instruction overhead (do_exec + op_Parallel + peripherals + runUntilTx) is about 31%. Batching
  the peripheral tick and the `runUntilTx` poll every N instructions is cheap (it's in the glue),
  but removing *all* of that overhead would cap out around 1.45x, matching Stage 2's 1.13x from
  removing dispatch alone.
- The port needs about 2.5x just to reach 100% of one core, and about 4x for a comfortable 60%. The
  interpreter can't get there. Only a full native-code JIT could, and Stage 2's gate already ruled
  that out as a bet.

**Decision: Monomodule on the Force is shelved.** The fork stays as a record. If revisited, the only
realistic path is a full native JIT for the hot handlers (Stage 3 as written), with the
register-pressure risk noted above.

## Static recompilation gate test (2026-09-26): 1.84-2.06x, registers match

Stage 2 only removed dispatch; the profile showed the cost is inside the handlers (runtime operand
decode, generic flag updates). So this test translates the same 10-instruction loop **ahead of time
into C++**: each instruction becomes a direct call to the interpreter's own handler with a *constant*
opcode, compiled into `dsp.cpp`'s translation unit (`dsp56k_recomp.inl`, via `__has_include`) so GCC
inlines the handlers and folds their decoding away. The handler for each PC is read from the
interpreter's own opcode cache after one interpreted pass (`DSP::getRecompInfo`), so resolution
matches the interpreter by construction. Tools: `tools/arm32jit_prototype/recomp/`.

- **Correctness: PASS.** Identical registers to the interpreter.
- **Stage 2's "a0/y0 swap" was a harness bug, not a compiler bug.** The loop writes `l:(r1)` and
  `y:(r0)+`, and the old harness reset registers but not memory, so the second run read the first
  run's writes. Once memory is restored too, it passes. Stage 2's block was probably correct as well.
  Separately, Stage 2 wrongly treated `$100097` (0x200034, ALU-only) as a parallel pair; the
  interpreter's cache doesn't.
- **Force timing (3 runs, 2M iterations): interpreter 62-66 ns/instr, recompiled 30-36 ns/instr,
  1.84-2.06x.** That passes the 1.3x gate Stage 2 failed, with no hand tuning at all.
- It is **not yet enough on its own.** At ~2x, the 242% average load would drop to ~120% of one core,
  still over budget. Remaining out-of-line calls in the generated code are memory access
  (`Memory::get`, `memWrite`, `decode_MMMRRR_read`) and `alu_asl`. DSP registers also still live in
  the `DSP` object rather than in locals across the block.
- **Next gate:** inline the memory fast path (MMU-backed direct array access) and see whether this
  loop reaches **>= 3x**. If it does, a whole-program recompiler (per-block, hash-checked, falling
  back to the interpreter, plus batching the peripheral tick and `runUntilTx` poll that cost ~11% in
  the profile) is a realistic weeks-scale project. If it stalls around 2x, stop.

### Second gate (same day): 3.73-3.83x with `flatten`, gate passed

Two changes, measured separately. Force, pinned with `taskset -c 3`, 5 alternating runs x 3M
iterations; the performance governor was already set:

| variant | interpreter | recompiled | speedup |
|---|---|---|---|
| inline memory fast path only | 70 ns/instr | 35-36 ns/instr | 1.94-2.04x |
| + `__attribute__((flatten))` on the generated block | 66 ns/instr | 17.3-17.8 ns/instr | **3.73-3.83x** |

Registers matched the interpreter on every run.

- **Inline memory fast path** (`memory.h`): with a valid MMU buffer, `Memory::get`/`dspWrite` are
  inline one-line array accesses; everything else goes to the renamed `getSlow`/`dspWriteSlow`.
  Behaviour is unchanged, and it helps the interpreter too. On its own it made no measurable
  difference to the recompiled block.
- **`flatten`** (emitted by `recomp_gen.py`) makes GCC inline everything reachable from the block:
  `decode_MMMRRR_read`, `DSP::memWrite`, `alu_asl`, plus the now-inline memory access. That's where
  the win is.
- Earlier, unpinned runs swung 60-98 ns/instr for the interpreter alone (MPC load on shared cores),
  so always pin with `taskset` for these measurements.

At ~3.8x, the 242% average interpreter load would come to ~64% of one core. That's close to the
~60% target, before the other planned savings: batching the peripheral tick and the `runUntilTx`
poll (~11% of the profile), and dropping per-instruction bookkeeping the block still does. Caveat:
this is one MAC-heavy loop, and a whole program adds block entry/exit, branches, hardware loops and
interpreter fallback.

**Next: whole-program recompiler.** Walk the DSP program into basic blocks from real executions
(same trick: the interpreter's opcode cache gives the handlers), generate one `flatten` function per
block, dispatch by PC through a table, verify each block's P-memory words at runtime (fall back to the
interpreter on mismatch or unknown PC), and handle DO/REP loops and branches at block ends. Gate for
that stage: `mnm-golden` hash-matches the interpreter on all 22 machines, and `mnm-bench` averages
<= 100% of one core on the Force.

## Whole-program static recompiler: first working version (2026-09-26)

**Result: correct on all 22 machines, Force load 225.5% -> 113.4% of one core (1.99x).** The gate is
<= 100%, so it isn't there yet.

How it works (`tools/arm32jit_prototype/recomp/`, dsp.h/dsp.cpp `recomp*`):
1. `mnm-recomp-discover` (a build with `-DDSP56K_RECOMP_DISCOVERY`, run with `MNM_DSP_INTERP=1`) runs
   mnm-golden's exact workload with a pre-execution hook in `execInterpreter`. It records each executed
   instruction (words, the handlers the interpreter's opcode cache resolved, block-ending kind, whether
   the parallel move needs the ALU/move latch, whether it reads PC), plus entry points and DO loop ends.
   Result: 9043 distinct instructions, no address ever holds two different instructions.
2. `recomp_gen2.py` builds basic blocks (1136 blocks, avg 7.6 instructions, covering 98.4% of executed
   instructions) and emits one `__attribute__((flatten))` `DSP::recompBlock<PC>` per block, plus
   `DSP::recompProgram()` (the block table, a PC index and a covered-address bitmap, all shared by every
   DSP instance).
3. A build with `-DDSP56K_RECOMP -I<dir of dsp56k_recomp.inl>`: `execInterpreter()` runs the block at the
   current PC instead of one instruction, when:
   - the block's P words match (verified lazily, and again after a P write inside it);
   - not in fast-interrupt mode;
   - no active DO loop ends strictly inside the block.

   Otherwise it interprets. DO/REP/WAIT/IFcc always go through the interpreter, and so do DO loops
   (do_exec calls execInterpreter recursively, so loop bodies dispatch to blocks). Interrupts and
   peripherals are checked once per block, and the instruction counter is advanced once per block,
   like the JIT.
4. The generated `.inl` contains firmware opcode words: never commit it (there's a `.gitignore`). Build
   it from your own OS `.syx`.

What's in a block: per instruction, direct calls to the interpreter's handlers with constant opcodes.
There's no PC or opcode-length bookkeeping except where a handler reads it (control flow, LRA, STOP),
and the final PC is set once at block end. Parallel instructions skip the latch unless the move
touches an accumulator the ALU writes (36.7% still need it).

Measured on the Force (SID machine, pinned): ARM instructions 10.1G -> 4.9G, IPC unchanged (~1.1),
L1 I-cache misses 21.7M -> 44.6M (the generated code is several MB), branch misses 110M -> 20M.
Profile: 76% in blocks, 6.9% do_exec, 2.7% runUntilTx, 2.6% peripherals, ~2% leftover interpreter.
Neither per-block instruction counting nor a P-write coverage bitmap made a measurable difference.

**Bit-exactness, and an unexplained difference:** recompiled builds match the x86 interpreter/JIT
hashes on all 22 machines, both on x86 and **on the Force**. The plain armhf interpreter still differs
on the 7 effect machines (see above). It isn't handler resolution: armhf discovery (under qemu)
resolves identical handlers at every PC to x86's. So it's something in the interpreter's
per-instruction path that the recompiled path bypasses; still open. Practically, the recompiled Force
build now produces the same audio as the x86/aarch64 builds.

Next candidates (bigger): specialise address-register updates on the M register values seen at each
instruction (with a runtime guard); run single-block DO loop bodies in a tight loop inside generated
code instead of via do_exec + execInterpreter per iteration; recompile REP'd instructions.

## Stage 3 progress (2026-09-26/27, overnight)

Every step below is bit-exact on all 22 machines (x86 and Force). Force median of 3 pinned runs
(`tools/arm32jit_prototype/recomp/bench.py`, noise about ±1% when MPC is quiet, ±5% otherwise):

| step | commit | avg load |
|---|---|---|
| whole-program recompiler | d6bc6250 | 113% (98-102% on a quieter device) |
| `alu_mpy`: 32x32->64 multiply (one `smull`) | f944993d | ~95% |
| dead-CCR elimination for MPY/MPYR/MAC/MACR (36% of executed instructions) | 3fdc3398 | 89.2% |
| DO loop's final body block run straight from do_exec (like the JIT's in-block loop) | 59a12d1c | 87.9% |
| register->memory parallel moves run before the ALU, no A/B latch | 8cc3a9dd | 85.9% |
| dead-CCR variants for ADD/SUB S,D and ASL/ASR #ii | (gen) | 85.8% |
| whole-DO-loop functions for single-block loop bodies (`recompLoop<PC>`) | 5dcedc92 | 78.7% |
| `limit_transfer`: one unsigned range check | 07076d6d | 73.2% |
| AGU linear-addressing fast path; peripheral branches unlikely | 269d6867 | 67.6% |
| 56-bit `signextend` via high word only (one `sbfx`); `scale()` single test | e1223e6f | 60.5% |
| HDI08 TX polled without acquire barriers (`RingBuffer::sizeSameThread`, glue patch) | c30aa221 | 59.1% |

(mnm-bench, normal priority, pinned; the interpreter was ~225% on the same measure.)

**Realistic load (2026-09-27): every machine's p99 is under 100%.** `mnm-spikes` with `MNM_PACE=1`
(sleeps to each 128-frame deadline, like an audio thread), `SCHED_FIFO` 70, CPU 3. Measured with the
user's normal background load running: a JV-880 emulator in MPC using most of another core, and
MockbaMod's capture script run once a second.

| machine | mean | p99 (128 fr) | p99 (512 fr) | max |
|---|---|---|---|---|
| DPRO DDRW (heaviest) | 66.3% | 93.0% | 89.3% | 103.6% |
| DPRO DENS | 66.6% | 91.5% | 87.9% | 105.4% |
| RINGMOD | 64.8% | 88.9% | 86.5% | 101.1% |
| REVERB / SID 6581 | 61.6% | 87.2% / 83.8% | 83.6% / 81.0% | 97.4% / 92.4% |
| lightest (GND SIN) | 38.0% | 54.0% | 51.6% | 60.8% |

Method notes, so these numbers get reproduced properly:
- **Don't benchmark `SCHED_FIFO` without pacing.** A FIFO thread that never sleeps hits RT throttling
  (`sched_rt_runtime_us` 950000/1000000 on the Force), which shows as 14-block bursts of ~1.5x-slow
  blocks once per wall-clock second.
- **Even paced, a ~40 ms 1.3x slowdown recurs once a second**, from the background (a system-wide
  `perf record -a` shows `capture.sh`/`mount`/`mkdir` spawned every second at the same phase, plus
  `jv880-emu` running continuously). This is what sets p99 on this device today.
- **The slowest blocks execute the same number of DSP instructions as the rest** (`slow1%` = 1.00x
  in mnm-spikes), so all the variance is environmental.
- **Where the time goes now:** `prof.sh` (a `-g` build profiled on the device, samples mapped to
  inlined source functions with `addr2line -i`) is the tool that found the last four wins. Hot now:
  - `alu_mpyT` 7.5%
  - `updateAddressRegister` fast path 7% (memory read-modify-write of R registers)
  - `limit_transfer` 4%
  - `isPeripheralAddress` 3%
  - SR bit set/clear/toggle ~6% combined
  - the outer dispatch (runUntilTx + execRecompiled + peripheral tick) ~8%

Lessons:
- **opcodeanalysis' register masks are incomplete.** Some MAC/MPY entries report no X/Y source
  registers at all. Trusting them to reorder parallel moves broke 10 machines, which `mnm-golden`
  caught. Decide safety from opcode bits (move direction W) and from handler names, never from those
  masks alone. The dead-CCR pass also has name-based barriers for this reason.
- **p99 spikes are not DSP work.** `mnm-spikes` (per-block wall time and DSP instruction count):
  the slowest 1% of blocks execute exactly the average instruction count. At normal priority, the
  benchmark is preempted by other processes (node servers, VNC, MPC). Under `SCHED_FIFO` 70 pinned
  to CPU 3, which is how an audio thread really runs, every machine's mean drops ~25%. Heaviest
  machines:
  - DPRO DDRW: mean 84%, p99 122%
  - DPRO DENS: mean 83%, p99 117%
  - RINGMOD: mean 82%, p99 120%
  - REVERB: mean 75%, p99 113%
  - Everything else: mean <= 70%.

  mnm-bench's numbers (normal priority) overstate the real load. The remaining p99 is
  micro-architectural (cache/TLB, shared L2 with MPC's cores), so the VST wrapper should run the DSP on
  its own RT thread with a buffer of lookahead, where the mean is what counts.

## Stage 3 conclusion (2026-09-27)

**Gate met in substance: the heaviest machine is at ~57-67% of one core** (FIFO unpaced / paced with
background load), p99 <= 93% for every machine, bit-exact. The written target was 50-60%; DDRW paced is
67%, the rest are at or under it. The runtime-JIT plan (Stages 2-4 as first written) is superseded by
static recompilation plus targeted interpreter fixes. Final step table:

| | mnm-bench avg (normal prio) | heaviest, paced RT mean / p99 |
|---|---|---|
| interpreter | ~225% | (not real-time) |
| recompiler v1 | 113% | |
| + Stage 3 | **57.6-58.4%** | **67.1% / 92.9% (DPRO DDRW)** |

Last two steps: `isPeripheralAddress` threshold compare (1dce4d55, 58.1% -> 57.6%); the dispatch stats
counter moved behind `DSP56K_RECOMP_STATS` (no measurable effect).

What's left if more headroom is ever needed (remaining hot spots per `prof.sh`):
- Mode-bit reads from SR (SM saturation, S0/S1 scaling, SC) on every operation. They can't be cached
  across a block because CCR updates write the same SR word. Fix: split MR/CCR storage, or specialise
  blocks by mode with a guard at entry.
- 48/56-bit register half updates with 64-bit masks (`loword`/`hiword`), and address-register
  read-modify-writes through memory. The real fix is keeping DSP registers in CPU registers across a
  block, i.e. rewriting the hot handlers for the generator (a true Stage 3 codegen).
- Block chaining (skipping dispatch between fall-through blocks): ~8% of time is outer dispatch. Risk: it
  changes interrupt/TX-poll granularity, so it needs the golden check.
- Dead-CCR across loop iterations (peel the last iteration); more dead-CCR variants.

Open items for the port itself:
- **Coverage:** discovery uses mnm-golden's script. Code paths it never runs (other parameters, patterns,
  machine switching) fall back to the interpreter: correct, but slower. Before shipping, trace a richer
  workload (all parameters and ranges, note ranges, LFO modes, switching) and merge traces.
- **Legal/distribution:** the generated code embeds firmware opcode words, so the plugin with
  recompiled blocks can't be distributed publicly. Build it per user from their own OS `.syx` (Docker
  pipeline), or ship the generator and have users run it.
- **The armhf plain interpreter still differs from x86 on the 7 effect machines** (unexplained; handler
  resolution ruled out). The recompiled build matches x86. It matters only for code the recompiler falls
  back on.
- VST wrapper: run the DSP on its own `SCHED_FIFO` thread (pinned, one buffer of lookahead); the
  p99-vs-window data above supports that.


## Coexistence with the rest of MPC (2026-09-27)

Question: can a Monomodule instance (one voice, one machine, as upstream designs it) run on the Force while it
keeps running other tracks and plugins? Setup on the Force (which had your normal add-ons running):
- MPC has one `AudioWorker` per core: SCHED_RR priority 20, each pinned to its own core (0-3). MPC's main/UI
  threads are on core 0 and its background/file threads mostly on core 1. `Audio Processing` runs on core 3.
- The current project barely loads the workers (~1%, 4%, 1%, 0.2% of cores 0-3).
- Your `mpc-vst-jv880` runs a `jv880-emu` thread inside MPC at SCHED_FIFO **45** on cores 0-2, ~20% of a core.
- Core 3 has no JV-880 and the audio-DMA interrupt; per-core single-engine results are near-identical
  (0.3-1.5% of blocks over deadline for the heaviest machine on any core).
- The kernel is PREEMPT_RT, governor `performance`, no cpuidle driver. Each engine costs 62 MB PSS (2 GB total).

**One engine per core (cores 1,2,3), each engine's own processing time** (mean% / p99% of the 2902 us block):

| other load per core, engine priority | DDRW 1 / 2 / 3 engines | SWAVE SAW 1 / 2 / 3 engines |
|---|---|---|
| none, FIFO 25 | 68/88, 65/89, 65/92 | 47/63, 46/64, 47/66 |
| 25% RR-20, FIFO 25 (above MPC workers) | 67/88, 64/87, 65/90 | 47/63, 46/64, 47/65 |
| 50% RR-20, FIFO 25 | 59/84, 60/83, 63/86 | 44/60, 44/61, 45/65 |
| 25% RR-20, FIFO **15** (below workers) | 98/151, 95/143, 111/175 (30% / 19% / 95% of blocks late) | 50/94, 55/93, 76/110 |
| 50% RR-20, FIFO 15 | 166/263 (all late) | 106/181 (70% late) |

So **N instances need N separate cores, and the DSP thread must run above MPC's AudioWorkers (priority > 20).**
Below them, even 25% background load causes misses. More engines than free cores is not an option for the
heavy machines (two engines on one core would need >130%); two typical ones (2 x 47%) would only just fit.

**Cost to the other work when the engine runs above it** (one engine, FIFO 25, on the same core as RR-20 work
that does a fixed amount of work per 2902 us period; percentage of the other work's periods that finish late):

| other load on that core | DPRO DDRW (66%) | SWAVE SAW (46%) | GND SIN (38%) |
|---|---|---|---|
| 10% | 0.8% | 0.03% | 0 |
| 20% | 1.6% | 0 | 0 |
| 30% | 6.8% | 0.16% | 0 |
| 40% | 2.9% | 0.07% | 0.09% |
| 50% | 25% (worst 53 ms) | 0.07% | 0 |

Reading it: typical machines coexist with up to ~50% other real-time work on their core. The heavy ones
(DPRO DDRW/DENS, RINGMOD, REVERB, SID) need a core mostly to themselves: other work is fine up to ~20% and
degrades past ~30%. The pattern is that the engine's slowest blocks (p99 87%, max ~115%) hold the core for
milliseconds, and any other work waking during them runs late.

Harness pitfalls found on the way (all cost me time, so noting them):
- **Makespan vs per-engine time.** `mnm-bench --engines` used to report only the block's makespan, which
  includes waking an unpinned CFS coordinator thread. With engines on core 1 plus another core that added
  ~25% of a period and looked like 33% deadline misses; each engine's own time was fine (mean 65%, p99 92%).
  Trust the "slowest worker's own time" line.
- **SCHED_FIFO below MPC's workers** looks like a performance problem when it's a priority problem.
- **Compute-only load on other cores does not slow an engine** (it measured *faster*, 60% vs 69%, with
  burners on other cores; not understood, possibly the DRAM/interconnect staying at a higher clock).

Not measured / open:
- How MPC actually calls a plugin: on its AudioWorker (RR 20) inside the audio callback, or on threads the
  plugin creates. Everything above assumes the plugin owns a FIFO thread above priority 20. A plugin doing
  the DSP inside `process()` on an AudioWorker at 66% of a core would leave that core's worker only ~34%.
- Real MPC projects (this used a synthetic fixed-work-per-period load, with the current near-idle project).
- The cost of idle-voice skipping (would cut the load of silent tracks; not implemented).
- Starvation risk: a FIFO thread that overruns starves the AudioWorker on its core (RT throttling caps it at
  95% per second). The wrapper needs a bailout, e.g. skip a block and output silence when behind.


## Gearmulator synths on the Force (2026-09-27)

Question: which other gearmulator (`dsp56300/gearmulator`, main @ 9710c1f) synths could use the static recompiler on
the Force? Names checked in the tree: Osirus/OsTIrus = `axel/`, Vavra (microQ) = `waldi/microq`, Xenia
(Microwave II/XT) = `waldi/xt`, Nord Lead 2x = Nodal Red 2x = `claudia/n2x`. No Nord Lead 1 support exists.
Method and tools: `tools/gearmulator_study/` (gm_probe: scripted MIDI workload on the whole device, x86 interpreter
build with `DSP56K_NO_JIT_RUNTIME`, plus the same probe cross-built for armhf and run on the Force, pinned to core 3
with every thread of the synth on that one core, so its wall/real-time figure includes the 68k µC and thread costs).
ROMs: user's own (Virus B/C .BIN, microQ 2.23 (byte-swapped dump, swapped back), Microwave II EPROM pair, Nord Lead 2x).
Not tested: Virus A (OS is two .mid files), OsTIrus (no TI ROM).

**Porting the core to these synths needed two things Monomodule never did** (both behind macros, off by default,
Monomodule build unchanged):
1. gearmulator has no interpreter path (it calls `getJit().exec()` and needs the JIT's config); the fork's opcode
   cache must be enabled (`DSP56K_INTERP_DEFAULT`).
2. **mQ, XT and n2x clock the ESAI from DSP *cycles*** (`ClockSource::Cycles`); the interpreter only counted
   instructions, so those synths hang at boot. `DSP56K_INTERP_CYCLES` adds the per-instruction cycle count
   (`calcCycles`, cached at resolve time). The generated recompiled blocks would have to add the same per-block sum.
   Also the JIT's `dynamicPeripheralAddressing` (wLib, patterns after `clr b M_AAR3,r2`) has no interpreter
   equivalent yet: not verified whether it matters for mQ/XT audio.

### Gate 1: cost (stopped here for everything except, conditionally, Vavra)

Executed instructions per audio second (interpreter, whole run incl. boot, 1 DSP unless noted) and the measured Force
interpreter time for the same run (all threads on core 3):

| synth | DSPs | DSP instr/s | of which idle spin-loops | Force interp (x real time) | projected after recompile (÷3.9) |
|---|---|---|---|---|---|
| Vavra (microQ 2.23) | 1 | 52 M | ~42% (4 addresses polling DSR0/TCSR2) | 6.55x | ~170%; ~100% if idle spins are skipped |
| Xenia (MW2/XT) | 1 | 59 M | ~13% | 7.71x | ~200% |
| Osirus Virus B | 1 | 107 M | ~59% (main loop at $29051/$e3c) | 8.37x | ~215%; ~120% with idle skip |
| Osirus Virus C | 1 | 98 M | ~45% (loop $2c084-$2c0a7) | not run | ~200%; ~110-150% with idle skip |
| Nord Lead 2x | 2 | 189 M total (95 M each) | small | not run (x86 3.0x) | ~250% per DSP thread |

Calibration: the Force runs about 8-9 M DSP instr/s per core in the interpreter (Monomodule: 21 M instr/s at 225%)
and about 36 M/s recompiled (21 M at 58%). The "20-25 M interpreter, x3.7" rule of thumb would put Vavra at ~65%,
so the Force run above (which includes the µC) is the number to trust: the interpreter is ~8 M/s here too.
Verdict per the rule (over ~100% after recompilation = stop): **Osirus, Xenia and Nord Lead 2x fail gate 1.**
**Vavra is borderline and only passes if idle spin loops are skipped** (a poll-loop fast-forward to the next
peripheral event; not implemented; changes timing of when the polled bit is seen, so it is not free), and the
68k µC (mc68k) adds to it. Gates 2-4 were not started for any of them.

### Gate 2 finding: no synth is deterministic run to run, even on the x86 interpreter

Two identical runs of gm_probe give different output hashes for mQ, XT and Virus B (5 s each). The µC thread, the DSP
thread and the audio thread run free (MIDI lands at a DSP position that depends on thread timing), so "hash the
x86 interpreter, require the recompiled build to match" does not work at the device level. It needs a lock-step
mode (DSP halted at frame boundaries, MIDI applied at frame counts) or a DSP-level record/replay of HDI/ESAI input.
Neither exists; that is the first piece of work if any of these are pursued.

### Known open issue check
The armhf-vs-x86 interpreter difference seen on Monomodule's effect machines could not be checked: the runs are
not deterministic (above), so armhf and x86 hashes differ for that reason alone.

### Other candidates
- `mo0kid/wave` (Waldorf Wave): a JUCE emulator built on 68000 cores and an ES2 ASIC model, no DSP56300. The static
  recompiler does not apply; it also needs `w2sys.bin`/`wdv.sys`, which are not in the roms folder.
- "Goldfinger": not in gearmulator main (no source, docs or commits mention it); needs a pointer.

### What each would need
- DSP thread pinning: one core per DSP (n2x two), SCHED_FIFO above MPC's AudioWorkers (RR 20) as for Monomodule; the µC
  thread (mc68k) also has to keep up, so a synth is really 2-3 threads.
- Memory: not measured on device (free memory was ~1 GB of 2 GB). The interpreter opcode cache is 48 B per P word;
  the P sizes of these synths were not checked.

### Follow-up (same day): Virus A, and idle-spin skipping for Vavra

**Virus A (OS 2.8, two .mid files):** 44 M DSP instr/s, flat profile (top address <1%: no idle loop to skip, unlike
B/C). Force interpreter, all threads on core 3: 4.99x real time -> ~130% after recompilation (÷3.9). Better than B/C,
still over 100% (and that's the DSP alone). Not friendlier enough on its own.

**Idle-spin skipping (`DSP56K_SPIN_SKIP`, off by default):** a run of >= 16 consecutive peripheral polls
(jset/jclr/brset/brclr on pp/qq addresses) confined to two addresses can only end when a peripheral changes, which
only happens when peripherals run, so the interpreter advances `m_instructions`/`m_cycles` straight to the
peripheral deadline. On Vavra this removes ~150 M of 260 M instructions per 6 s (52 -> ~35 M/s executed); what is
left is real DSP work (MAC/MPY loops at $223-$228, $6dd-$6e7, $963-$965). Force, all threads on core 3, 4 s of audio:
interpreter 6.55x -> 5.75x. Per-thread CPU on the Force for that run: DSP thread 20.3 s, 68k µC thread 6.3 s,
audio thread 0.9 s. So on the Force the µC is ~25% of the total, and the spin-skip helps less than the instruction
count suggests because the DSP thread's remaining time is real work. Projection with the recompiler (÷3.9 on the
DSP part only): DSP thread ~ 90-110% of a core (34 M instr/s vs ~36 M/s), µC thread on a second core.
That is still not comfortable; Vavra is "marginal on two cores", not "fits".
The poll instructions are never recompiled (they must run through the interpreter for the detector), which is the
right choice for a spin anyway.

**Why gate 2 (bit-exactness) is blocked, precisely:** the µC (68331) runs on its own free-running thread; it is
clocked from the ESAI frame count the DSP thread has reached (`wLib::Hardware::syncUcToDSP`), so how many frames the
µC sees per slice, and where MIDI lands relative to the DSP, depends on thread timing. A deterministic device needs
the µC stepped at fixed points of the DSP's timeline. Options: (1) run the µC inside the DSP thread's ESAI callback
(one thread, deterministic, and no spin/yield loops, which is also what the Force wants) but the µC's DSP reset
request currently terminates the DSP thread from the µC side, which would join itself; (2) keep two threads but
hand off per ESAI frame via the existing halt-DSP mechanism (deterministic, ~90k context switches/s). Not done.

**Wave (mo0kid/wave) prerequisites:** the user's own Wave OS 1.700 files `w2sys.bin` and `wdv.sys` (from Waldorf's
public Legacy Wave System.zip; the loader checks hashes; wavetable images optional). It is 68000 code plus an ES2
ASIC model, so the DSP recompiler is irrelevant; the question there is the cost of the 68000 cores and the ASIC
voice model (its README mentions 3 worker threads at high polyphony) and whether the JUCE build works on Linux/armhf
(its targets are macOS AU/VST3/AAX plus standalone).

## Vavra lock-step + recompiler attempt (2026-09-27, later)

Goal: a deterministic single-thread build of Vavra (needed so the recompiled build's output can be hashed
against the interpreter's, per the correctness gate), then run the recompiler on it.

**Lock-step mode (`GM_LOCKSTEP`, off by default) works and is bit-exact.** `wLib::Hardware` gets a
`lockstepStepDsp()` hook; `mqLib` runs the uC and DSP on one thread, the uC's own thread disabled, every
`ucYieldLoop`/wait call replaced by stepping the DSP directly, and the ESAI's blocking ring-buffer callback
replaced by a non-blocking one (`MqDsp::onDspBootFinished`). A budget in DSP cycles (converted to uC cycles via
the ESAI clock's cycles-per-sample, x2 for two slots per frame) decides whose turn it is
(`Hardware::lockstepRun`/`lockstepStepDsp`). Verified deterministic: identical output hash across repeated runs,
on x86 and on the Force (interpreter build only -- see below). Sample: 10s render at
`tools/gearmulator_study/` scratch, sent to the user.

**The recompiler does not work on Vavra yet -- a real, reproducible correctness bug, not yet fixed.** The
recompiled + lock-step build gets stuck in a genuine infinite retry loop at P:$013b2-$013ed (an HDI08
host-command poll) that the interpreter's own discovery trace shows executing only 10-45 times in a full
reference run. Confirmed with instrumentation (periodic dumps of DSP/uC cycle counts, PC, budget, boot-reset
count): both builds hit the SAME sequence of boot-time DSP resets at the SAME uC cycle counts (0->1->2->3,
identical between interpreter and recompiled -- this part of the protocol is uC-only and unaffected by DSP
recompilation). The divergence starts exactly when the DSP begins REAL execution after the 3rd reset: the
interpreter passes through the poll quickly and boots; the recompiled build enters it and never leaves (still
looping after 75M+ DSP cycles, `running=1`, PC cycling through the same ~10 addresses, budget still trading
turns normally with the uC -- so it is not a scheduler starvation bug).

Ruled out by direct experiment (each rebuilt and rerun, still hangs):
- **Block coalescing / once-per-block peripheral ticking** -- forced `MAX_INSTR=1` (every block is a single
  instruction, closest possible to interpreter granularity). Still hangs.
- **`movep` (the actual HDI08-register instructions in the loop)** -- forced kind=2 (interpreter-only,
  never recompiled) for every instruction whose handler name contains `Movep`. Still hangs at the exact
  same PCs, now via the interpreter fallback, which rules out a bug in generating movep specifically.
- **Self-modifying code / bank-switched instruction words** -- checked the discovery trace used to build this
  `.inl`: zero PCs hold more than one distinct opcode word (the Monomodule assumption holds for this trace,
  unlike a wider 40s trace taken later which does have ~47M such conflicts elsewhere, but that trace was never
  used to generate this `.inl`).
- **Dead-CCR elimination** (`recomp_gen2.py`'s known-fragile pass, flagged in its own comments as relying on
  incomplete opcode masks) -- disabled entirely (`is_killer` forced false). Still hangs.

Remaining suspects, not yet tested: the per-block DSP-cycle summation added for this synth
(`DSP56K_INTERP_CYCLES`, needed because mQ/XT/n2x clock the ESAI from DSP cycles and the plain interpreter
never counted them) -- unlike everything else in the recompiler, this is new code, not carried over from the
Monomodule-proven pipeline, and a per-block-summed cycle count could show a stale value to an instruction
mid-block that reads the clock to make a timing decision, even though MAX_INSTR=1 should have equalised that
granularity (still hung, so this alone may not explain it either -- needs the actual differential/single-step
comparator Stage 1 used for Monomodule, which was not built for this session).

Everything needed to resume this is in `tools/gearmulator_study/`: `recomp_gen_gm.py` (the gearmulator variant
of `recomp_gen2.py`, adds the DSP-cycle column and the Movep exclusion), `gm_probe.cpp` (`GM_LOCKSTEP`,
`GM_DISCOVER`, `GM_HOT`, `GM_WIDE`, `GM_LSDEBUG` env vars document themselves at point of use), and
`gearmulator-glue.patch` (the full diff against `dsp56300/gearmulator` main, including the lock-step wiring).

**Next step if this is picked up again:** build the Stage-1-style differential comparator (run interpreted and
recompiled DSP instances side by side from identical state, diff full registers after every instruction, not
just instruction counts) to find the exact first point of divergence, rather than continuing to guess-and-check
whole mechanisms.

## Vavra recompiler: the hang is fixed (2026-09-27, later still)

**Root cause found via a differential comparator (per-instruction pc/a/b/sr trace, x86 interpreter vs
recompiled, diffed index-for-index -- built for this, see `tools/gearmulator_study/gm_probe.cpp`'s
`GM_REGTRACE`/`GM_REGTRACE_CAP`).** First divergence was at instruction #4,117,125: the interpreter goes
`P:$013b1 -> $013b3` (a 2-word `BRCLR #n,S,label` self-poll branch, correctly consuming its label
extension word); the recompiled build goes `$013b1 -> $013b2`, treating the label word as a separate,
wrong instruction.

**The bug was in the discovery tool, not the DSP core or the recompiler's code generation.** Its opcode
decode (`Opcodes::getInstructionTypes`/`findNonParallelOpcodeInfo`) uses a per-instance table
(`m_opcodesNonParallel`) built by the `Opcodes()` constructor from `hasField()` checks; discovery used a
namespace-scope global `Opcodes g_ops;`, constructed at static-init time (before `main()`), while the DSP's
own copy is a member constructed later, after the ROM loads. For this ambiguous bit pattern (0x0cc300,
which superficially also matches `Add_SD`'s near-fully-wildcarded encoding), the global's construction
raced/preceded something the field tables depend on and picked the wrong candidate, silently. The real DSP's
own copy of the same table (built after full program init) always resolved it correctly, which is why the
disassembler and every live-executing build got this right and only the discovery-time re-decode got it
wrong. Fix: made the `Opcodes` instance a function-local (lazily-constructed) singleton
(`Opcodes& ops() { static Opcodes instance; return instance; }`), guaranteeing it isn't built before
whatever it depends on. One-line fix; `tools/gearmulator_study/gm_probe.cpp`.

**Result after fixing and regenerating the trace + `.inl`: the recompiled + lock-step build completes.**
No more hang, on x86 or the Force. Deterministic (same hash across repeated runs). mnm-bench-style timing:
x86 0.60-0.66x real time (faster than real time); Force (pinned, taskset -c 3) 4.75x real time -- an
improvement on the interpreter's 6.55x but not yet real-time on-device, expected since a nontrivial share
of Vavra's boot/handshake code (2-word branches like the one above) is correctly excluded from
recompilation (`kind=2`, interpreter fallback) and this trace hasn't had the Stage-3-style optimisation
pass Monomodule got.

**Not yet bit-exact: a second, different divergence remains, further into execution.** Re-running the same
differential comparator (now index-1,000,000+ before it recurs) finds the SAME instruction (`BRCLR` self-poll,
this time at $013dc, correctly classified `kind=2`/interpreter-only after the fix) where the interpreter's
poll condition resolves (branches out to $013de) while the recompiled build's identical interpreter-dispatch
call for the same instruction keeps looping. Since kind=2 means this exact instruction runs through the
plain interpreter in both builds, the difference isn't in decoding it -- it's that the *peripheral state it
polls* differs, meaning the uC/DSP lock-step scheduling has drifted by this point. Leading suspect: the
per-block `DSP56K_INTERP_CYCLES` cycle summation (added for this synth, not part of the Monomodule-proven
pipeline) accumulating a small error over millions of blocks, enough to shift when
`Hardware::lockstepRun`'s uC/DSP turn-taking (driven by DSP cycle count) hands control to the uC relative to
real interpreter timing. Not yet confirmed or fixed -- worth its own differential trace pass (log the uC's
own cycle count alongside DSP pc/regs) before touching the cycle math.

Housekeeping: `tools/gearmulator_study/recomp_gen_gm.py` now includes the fixed length column) and the
Movep-exclusion no longer matters (superseded by the real fix, kept anyway as a defensive default). New
files: `recomp_gen_gm_max1.py`/`_max1_noloop.py` (diagnostic MAX_INSTR=1 variants, useful for future
differential work), `gm_probe.cpp`'s `GM_REGTRACE`/`GM_REGTRACE_CAP`/`GM_LSDEBUG` are now permanent tools,
not one-off hacks.

Sent to the user: a 10s render from this fixed recompiled build, and one from the plain interpreter for
comparison (same script). Both complete; correctness between them is not yet proven bit-exact per above.

## Second divergence, narrowed further (2026-09-27, still later)

Re-ran the differential comparator (now with the DSP's own cycle count added to each trace record,
`GM_REGTRACE`'s `Rec` gained a `dspCycles` field) past the first fix, to check the cycle-accounting theory
directly. **Cycle counts match exactly, instruction for instruction, right up to the divergence** (both
builds show identical `dspCycles` at every matching PC through a ~2.88M-iteration hot loop at P:$000963-965) --
this rules out per-block `DSP56K_INTERP_CYCLES` summation error as the cause of this second divergence.

The actual divergence: at the exact instruction where the interpreter falls through the loop's last body PC
($000965, a `DO` loop end) to $000966 (the instruction immediately after the loop, executed 8,382 times
total vs. the loop body's 2.88M), the recompiled build skips $000966 entirely and lands on $000967 instead.
$000966 does have its own generated block (confirmed present in the `.inl`, correctly a leader since it's
`loopend+1`), so it isn't simply missing from generation.

Narrowed to (not yet pinned exactly): `DSP::execRecompiledLoopBody()` only runs a recompiled block as a
loop-body step when that block's own span reaches exactly `la+1` (`b.pc + b.numWords != reg.la.toWord() + 1`
=> `return false`); a 1-to-3-instruction non-loop block within the loop body (this loop's block at $000963 is
not a `recompLoop`, `loop_body_ok` apparently rejected it) fails that check and falls back to plain
`execInterpreter()` for every iteration -- which should be equivalent, and cycle counts through the loop
confirm it is. The actual skip happens at the boundary where `do_exec()`'s own loop-exit sets PC to `la+1`
and returns; suspect is in how control resumes into recompiled code after that return (if the `DO` instruction
that started this loop was itself reached from a recompiled block, whatever that block does with PC after
`op_Do_xxx()` returns needs checking), but this was not confirmed before time ran out on this pass --
next step is to trace whether the code immediately preceding P:$000963's loop is itself recompiled, and if
so read that specific generated block's handling of the `DO` instruction and what it does with PC afterward.

Practical effect: this is a **narrow, boot-time-only skip** of a rarely-taken (8,382 of ~2.9M) post-loop
instruction, not a hang and not (as far as tested) audible corruption -- the build completes, is internally
deterministic, and produces plausible audio (sent to the user). It should be fixed before calling Vavra's
recompiler bit-exact, but does not block further real-world testing.

## Second divergence, continued (2026-09-27, still later): tracer bug fixed, then a real remaining one found

**Fixed a real bug in the diagnostic tool itself.** `GM_REGTRACE`'s hook only fired from plain
`execInterpreter()`; `execRecompiledLoopBody()` and `execRecompiledLoop()` bypass that function entirely
(they call the resolved block/loop function directly), so any instruction executed via those fast paths was
invisible to the trace. This made the earlier-reported "$000966 skipped" divergence a **false positive**:
with the hook added to both fast paths (`dsp.h`, gated by the existing `DSP56K_RECOMP_DISCOVERY` macro, kept
permanently), a re-run confirmed the recompiled build executes P:$000966 correctly, every single time
(8000+ direct confirmations via a separate `do_exec()`-level instrumentation pass), and the two builds match
bit-for-bit, cycle-for-cycle, for the first 15 million instructions.

**A real divergence remains further in.** Extending the corrected comparator to 100M instructions finds it at
instruction #16,261,375 (well past the first 15M that matched): the DSP's own cycle *and* instruction
counters are identical between builds at this point, then a single instruction's cost differs by a small,
non-repeating amount (10 cycles / 2 instructions), and PC trajectories fully diverge soon after (full-program
divergence within ~10M more instructions). A second, independent occurrence (found on a build without
`DSP56K_SPIN_SKIP` at all) shows the same signature at a different, simpler-looking spot: two builds at
*identical* DSP cycle and instruction counts take different branches on the exact same `jset`/`jclr`-style
poll instruction (P:$0001db, itself always interpreter-only, `kind=2`) at instruction #13,262,919.

**Ruled out by direct experiment** (each rebuilt and rerun to a full audio hash comparison, not just the
first differing index): `DSP56K_SPIN_SKIP` (disabled entirely, still diverges elsewhere); whole-loop
recompilation (`recompLoop`, disabled via `recomp_gen_gm_noloop.py`, still diverges); block-size/granularity
(forced `MAX_INSTR=1`, so every `exec()` call is exactly one instruction like the interpreter, still
diverges). None of these change the outcome, which rules out "the recompiler runs bursts the lock-step
scheduler can't interrupt" as the explanation.

**What's left, precisely:** at the second divergence, the instruction is `kind=2` (always interpreter-only,
identical code path in both builds) and DSP-side cycle/instruction counters match exactly, yet it evaluates
differently -- meaning the *peripheral/memory value it polls* differs, which can only come from something
outside the traced DSP registers: most likely the 68k microcontroller having taken a different number of
turns by this exact DSP-cycle count. The uC side isn't instrumented at all in this trace; that's the natural
next step (log the uC's own PC/cycle count alongside the DSP trace) rather than continuing to guess at DSP-
side mechanisms, which are now fairly thoroughly excluded.

**Housekeeping:** `GM_REGTRACE`'s record gained a `dspInstr` field (the DSP's own instruction counter,
alongside pc/a/b/sr/dspCycles) -- keep it, it was essential for this pass. The one-off `DSP56K_DOEXEC_DEBUG`
instrumentation used to confirm the tracer bug was reverted (not kept); the `s_recompTraceHook` calls added
to `execRecompiledLoopBody()`/`execRecompiledLoop()` in `dsp.h` are kept permanently -- any future
recompiler diagnostic work depends on the tracer actually seeing every instruction.

## uC side instrumented, and the 68k-timing theory is now disproven (2026-09-30)

**Instrumented the 68k side.** Added `mc68k::Mc68k::s_traceHook` (`source/cpu/mc68k/mc68k.h`/`.cpp`, gated by
`DSP56K_RECOMP_DISCOVERY`), called once per uC instruction from `Mc68k::exec()` with its PC and cycle count --
mirrors the DSP-side hook. `gm_probe.cpp`'s `GM_REGTRACE` writer gained `GM_REGTRACE_UC=1`, which installs it
and interleaves DSP and uC events into one trace file in true call order (`kind` byte: 0=DSP, 1=uC), so the
scheduling order itself can be compared between builds, not just each side's own counters. Committed as
`388914cb`.

**Ran it: 100M-record interleaved traces, interpreter vs. recompiled, both built with
`-DDSP56K_RECOMP_DISCOVERY -DGM_LOCKSTEP` (recompiled also `-DDSP56K_RECOMP -I<fixed .inl dir>`).** First
diff attempt used a naive Python script that materialized every record into a list of tuples -- with ~100M
records per trace this meant tens of GB of Python object overhead, which appears to have crashed the host
VM outright (not just the process) at least twice, wasting real time before the actual cause was identified.
Rewrote as an `mmap` + generator two-pointer diff (`diff_uc3.py`, kept at `tools/gearmulator_study/` -- see
below) with flat memory use; that ran cleanly to completion as a background task.

**Result: the uC event stream DOES diverge eventually (at uc-event #15,667,848: interpreter at
PC=$089f82/cycles=63949182 vs. recompiled at PC=$089f8e/cycles=63949180) -- but this is downstream of, not
the cause of, a DSP-side divergence found earlier in the same trace (by file position).** At DSP instruction
counter 13,179,646, both builds show identical DSP cycle count (20,724,672), identical `sr`, identical `a`/`b`
registers -- but the interpreter's PC is $0001e6 while the recompiled build's is still $0001db. $0001db is the
same `kind=2` poll instruction identified in the previous session (always interpreter-only, byte-identical
machine code in both builds). Since the exact same C++ handler executes in both builds for this instruction,
and every DSP register the trace captures matches, the only way it can branch differently is if **a
memory/peripheral value it polls (not captured by the a/b/sr/cycle trace) differs between builds at this
point** -- not an instruction-decode bug, not a cycle-accounting bug, and (per the uC evidence, which diverges
*later* in the trace) not a 68k-side scheduling drift causing this. The 68k-timing hypothesis from the
previous session's writeup is therefore disproven as the root cause; the 68k drift is a symptom that shows up
after the fact, once the DSP has already gone down a different path.

**Not yet found: which earlier instruction writes the wrong value to whatever memory/peripheral address
$0001db polls.** That's the next concrete step -- extend the DSP-side trace record to also capture the
specific memory word(s) $0001db reads (need to identify the instruction/operand first, e.g. via
`dumpAssembly`/disassembly at $0001db in the discovery output) and trace writes to that address going
backward from instruction #13,179,646 to find the first build-dependent write. Block-granularity and
whole-loop coalescing were already ruled out for this exact bug in the previous session (MAX_INSTR=1 combined
with no-loop still reproduced it), so the miswrite -- if that's what it is -- isn't a peripheral-tick-timing
artifact of block coalescing; it's more likely a genuine miscompilation of one specific opcode that only
shows an observable effect this far into execution.

New file: `tools/gearmulator_study/diff_uc3.py` (the mmap/generator differential comparator used for this
pass -- memory-safe for 100M+-record traces, unlike a naive list-based version). Trace files themselves
(`trace_int2.bin`, `trace_rc3.bin`, ~4.8GB each) are not committed -- kept on real disk at
`/home/sam/scratch-gm/`, regenerable via the build+run steps in `README.md`.

## Identified the polled register, ruled out a data-corruption theory, and found a methodology caveat (2026-09-30, later)

**Identified the exact instruction and register.** Added a one-off `GM_DISASM=<hex pc>` diagnostic to
`gm_probe.cpp` (uses `dsp56k::Disassembler` against the live DSP's P: memory after boot; kept, it's generally
useful). $0001db is `jset #$15,x:<<$ffff87,func_0001e4` -- a poll on bit 15 of **Timer2's TCSR**
(`$FFFF87` = `M_TCSR2` in `timers.h`). Bit 15 is `M_PCE` (Prescaled Clock Enable), a plain software-driven
control bit, not an automatic hardware status flag -- so the initial theory was a data-write bug (something
upstream writing the wrong value), not a timing artifact. A full-P-memory disassembly scan found every
reference to `$ffff87`: a `CLR B` + `MOVEP B,X:$FFFF87` pair at $0001d1-$0001d5, six instructions before the
poll, plus a couple of `BSET #$0` (setting `M_TE`, timer enable) elsewhere.

**Added `GM_TCSR2LOG`, a tiny env-gated diagnostic in `timers.cpp`** that logs every observed change to
Timer2's TCSR (both software writes via `writeTCSR()` and hardware-driven changes via `execTimer()`), tagged
with PC and the DSP's instruction counter. Ran both builds for 8 audio seconds spanning the previously-found
divergence point (regenerated the discovery trace + `.inl` from scratch this session, since `/tmp` had been
wiped by a host restart -- see "Housekeeping" below). **Result: the write VALUES and total event COUNT are
identical between builds (57,923 events each, same alternating `val=000001`/`val=300000` sequence, same
order)** -- this rules out a data-corruption/wrong-value-written theory for this specific register. Zero
`execTimer(hw)` events ever fired in this window (Timer2's `M_TE` gets set then cleared again within ~10-12K
instructions each cycle, too short to reach its own overflow/compare thresholds) -- so Timer2 is being used
here as a software-toggled flag, not a real hardware timer, and its own internal ticking logic isn't in play
for this specific register at this point in the program.

**However: the logged PC differs between builds for the exact same logical write event** (`pc=00012f` vs
`pc=00012a`, a constant 5-word offset, for what's otherwise the identical `BSET #$0` write) **and instruction
counts jitter by up to ~16 either side of the interpreter's**. This, plus revisiting the previous divergence
mechanics, points to a **methodology caveat in the differential trace itself**: the recompiled build's
`s_recompTraceHook` fires once per *fused block* (`recomp_gen_gm.py`'s blocks average 3.1 real instructions
each for this ROM), stamped with the block's *starting* PC, while the interpreter's fires once per real
instruction. Matching trace records by raw `dspInstr` counter value (as `diff_uc3.py` and the previous
session's analysis did) does not guarantee comparing the same real moment -- a recompiled block's one trace
record can represent several real instructions' worth of state change collapsed into one entry. The earlier
"DSP MISMATCH at dspInstr 13,179,646: int PC=$1e6 vs rc PC=$1db" finding is very plausibly this artifact
(the interpreter genuinely at $1e6 after several more branches; the recompiled build's *block-start* PC still
reading $1db while its block has actually progressed further internally) rather than proof of one instruction
branching differently on identical state.

**The bug is still real, though -- confirmed directly at the audio level, independent of any tracing
methodology.** Running both builds for a plain 20-audio-second render (no discovery/trace overhead) gives
different hashes: interpreter `df13dfba3901a669`, recompiled `c793b429768eb629`. So there is a genuine
functional divergence somewhere in this run; it just isn't yet pinned to a single instruction the way the
$0001db lead seemed to promise.

**Next step, revised:** stop trying to line up per-instruction traces across builds with different natural
granularities. Instead, bisect on the *audio output* directly (which needs no instruction-level
instrumentation and is granularity-agnostic): run both builds for progressively shorter durations / compare
sample-by-sample (not just a whole-run hash) to find the first audio sample that differs, then correlate that
sample's timestamp back to a DSP cycle count / instruction count via the existing per-cycle ESAI clock math to
find the surrounding code region. This sidesteps the block-vs-instruction trace mismatch entirely.

**Housekeeping:** the scratchpad (`/tmp`, tmpfs) was wiped by a host VM restart between sessions -- rebuilt
the gearmulator checkout, reapplied the fork + `gearmulator-glue.patch`, and discovered the `git apply` had
silently no-op'd on `source/cpu/mc68k` (a nested submodule) despite reporting "Applied patch...cleanly" --
had to reapply the `Mc68k::s_traceHook` edit by hand. Worth remembering if `gearmulator-glue.patch` is ever
reapplied fresh again. The `GM_DISASM` env var (disassemble a P: memory window around a given PC after boot)
and `GM_TCSR2LOG` (log Timer2 TCSR changes) are both kept in `gm_probe.cpp`/`timers.cpp` as permanent,
env-gated diagnostics -- zero cost unless the env var is set.

## Found it (probably): the divergence is at sample #730, and it's the DO-loop flag (2026-09-30, later still)

**Bisected on raw audio output instead of instruction traces** -- exactly the plan from the previous section,
and it worked immediately. Dumped both builds' interleaved-float PCM output (`gm_probe`'s existing
`dump.raw` argument) for a 20s render and compared sample-for-sample: **the two builds first differ at
output sample #730 (16.5ms into the render, audio block 11 of 64-sample blocks)** -- not at instruction
13-16M as the earlier (block-granularity-confounded) instruction trace suggested. Confirmed it's a genuine
value difference, not a latency/phase offset: no sample shift in a +/-5 window brings the streams back into
agreement.

**Correlating this to DSP state needed a new tool.** `execCountAll()` (the interpreter instruction counter
used for progress logging) is *not* comparable between builds -- it read ~36.8M for the interpreter vs ~7.0M
for the recompiled build at the exact same point (their loop-fusion/spin-skip accounting differs, apparently
substantially, though final audio was still identical up to sample 730 despite this). `getCycles()` (the
actual DSP hardware clock), by contrast, matches almost exactly between builds at every block boundary --
it's the only reliable common ground-truth clock. Sample #730 corresponds to roughly cycle 319,780,000-
319,952,000.

**Added `GM_REGTRACE_CYCLESTOP`/`GM_REGTRACE_RINGSIZE`** to `gm_probe.cpp`'s regtrace writer: instead of a
flat record-count cap (which would need ~150M+ records just to reach cycle 320M, most of it wasted boot-phase
history), it keeps a ring buffer of only the most recent N records and dumps it once the DSP's cycle count
crosses a target -- gives "the last N instructions leading up to cycle X" in a small file regardless of how
long boot took to get there. Kept permanently, zero cost unless `GM_REGTRACE_CYCLESTOP` is set.

**First attempt at matching records by nearest absolute cycle value ran into a real aliasing hazard**: the
code in this region is a tight, highly repetitive polling loop, so many records share nearby/identical cycle
values across very different loop iterations, and naive nearest-cycle matching can compare two different
*phases* of the same repeating loop rather than the same real moment -- worth remembering before trying this
match strategy again. Despite that caveat, the mismatch found this way was strikingly consistent: at two
different, widely-separated sample points in the ring, the interpreter and recompiled builds' `sr` (status
register) values differed by **exactly one bit, `0x8000`, which is `SR_LF` (`registers.h`) -- the DSP's DO-loop-
active flag.** Confirmed twice independently (same exact XOR both times), with `a`/`b` accumulators matching.

**This is a strong, mechanistically well-supported lead, not yet confirmed as the root cause.** `SR_LF` isn't
cosmetic: `DSP::execRecompiled()` (`dsp.h`) has `if((reg.sr.var & SR_LF) && TWord(reg.la.var - _pc) <
b.numWords - 1) return false;` -- i.e. whether a given PC dispatches via the fast recompiled path or falls
back to the interpreter *depends on this exact bit*. If the two builds disagree about whether a DO loop is
currently active, they can take different dispatch paths for the identical PC, which would very plausibly
cascade into the kind of small, hard-to-pin-down divergence chased across two sessions now.

**Next concrete step:** find where/why `SR_LF` ends up disagreeing -- likely somewhere in the DO/ENDDO
instruction handling or `do_exec()`'s own loop-entry/exit bookkeeping, specifically whichever code path
sets/clears `SR_LF` and whether the recompiled dispatch's loop-entry (`execRecompiledLoop`) or loop-exit
(inside `do_exec()`) keeps it in sync with the interpreter's own DO/ENDDO handling in every case. A direct,
tractable next diagnostic: log every write to `SR_LF` (both set and clear, with PC and cycle) the same way
`GM_TCSR2LOG` did for the timer register, and diff that between builds around cycle 319.8-320M.

**Checked the generated `recompLoop` bodies in `recomp-mq/dsp56k_recomp.inl` directly** (regenerated this
session -- the discovery trace and `.inl` don't survive a `/tmp` wipe, see Housekeeping above): the loop-exit
tail is faithful to `do_exec()`'s own logic and correctly delegates to the *shared* `d->do_end()` (not a
reimplementation) --
```cpp
if(!(d->reg.sr.var & SR_LF)) { ...; d->reg.pc.var = <after>; return true; }
if(d->reg.lc.var <= 1) { ...; d->setPC(<after>); d->do_end(); return true; }
```
matching `do_exec()`'s `if(!sr_test_noCache(SR_LF)) break;` / `do_end()` pair exactly, and the loop-entry
`sr_set(SR_LF)` + `ssl(reg.lc)` push happens in the *shared* `do_exec()` before it ever calls
`execRecompiledLoop()` (`dsp.cpp` line ~579, before line 589) -- so single, non-nested DO loops look
architecturally sound in both paths; this isn't an obvious reimplementation bug.

**Leading hypothesis, not yet confirmed:** a *nested* DO loop -- if a `DO` instruction is encountered from
*inside* a recompiled loop body's fused block, does the generated code correctly re-enter `do_exec()`'s
stack-based push/pop bookkeeping the way the interpreter naturally would by just executing the `DO` opcode
through the normal interpreter dispatch? If the code generator's fused loop body doesn't handle an inner `DO`
by genuinely calling `do_exec()` again (recursing properly, preserving the outer loop's saved `SR_LF`/`LC`/`LA`
on the software stack), a nested loop scenario would be exactly the kind of rare, hard-to-trigger case that
surfaces ~16ms into a run rather than immediately at boot. Next session: find whether this ROM's code near
sample #730's timeframe (cycle ~319.8-320M) has a nested DO loop, and if so whether it's inside one of the
`recompLoop` blocks in the `.inl` (check that block's generated body for how it handles an embedded `Do_xxx`
opcode, if it contains one at all -- `recomp_gen_gm.py`'s leader/block-boundary rules may already exclude DO
instructions from being fused into an enclosing loop body, in which case this hypothesis is wrong and the
`GM_TCSR2LOG`-style direct SR_LF-write logger is the more reliable next step regardless).

## The actual fork point, found (2026-09-30, later still)

**Ruled out the nested-DO-loop hypothesis directly.** `loop_body_ok()` in `recomp_gen_gm.py` already excludes
`Do` (among other unsafe opcodes) from ever being fused into a `recompLoop` body -- a nested loop just isn't
eligible for the whole-loop optimization in the first place, so it can't be a reimplementation bug there.

**Added `GM_LFLOG`** (env-gated, `dsp.cpp`, kept permanently): logs every `do_exec()`/`do_end()` call with
PC, loop-end address, stack count (`sc`), `SR_LF`, cycles and instruction counter. Ran both builds for 1 audio
second (942,198 vs 942,225 loop events -- close but not identical, confirming *something* eventually
diverges) and diffed the sequences structurally (loop nesting depth, PCs, `SR_LF` transitions, ignoring the
already-known-incomparable `instr` field). **Result: the two builds match in perfect lock-step for 410,135
consecutive loop entry/exit events** -- an enormous, reassuring amount of correctly-matched history -- **then
at the very next `do_exec`, they enter completely different loops** (`$0003ce` vs `$0002c3`). The last shared
event is `do_end` at PC `$0001ad` (the tail of `recompLoop<0x0001a8>`, a small 16-iteration DO loop), cycles
~319,549,090 (int) / ~319,548,555 (rc) -- a much more precise target than the earlier sample-730 estimate.

**Captured a tight per-instruction ring right at that cycle** (`GM_REGTRACE_CYCLESTOP=319560000`,
`GM_REGTRACE_RINGSIZE=20000`, using the `GM_LFLOG` binaries which already have `DSP56K_RECOMP_DISCOVERY`).
Locating the specific occurrence of PC `$01ae` (the loop's exit target) nearest the known cycle, then reading
forward: **int's real per-instruction PC sequence is `...457, 465, 3220...`; rc's per-block sequence is
`...457, 459, 603, 628, 633, 656, 682, 686, ..., 759, 462, 463, 465, 3220...`** -- both eventually reach PC
3220, but **with different `a` register contents there** (`71972656764682240` vs `72057594021150720`) --
confirmed genuine data divergence, not a block-granularity logging artifact.

**Disassembled the fork point exactly**: `$0001c9: brset #$0,y:$6,func_0001d1` -- tests bit 0 of `Y:$6`.
- **int** finds it *set*: jumps straight to `$0001d1` (`jsr func_000c94`).
- **rc** finds it *clear*: falls through to `$0001cb`, sets up `r0`, calls `jsr func_00025b` (a whole
  subroutine -- exactly matching rc's observed detour through `$25b`-`$759`), which does real work (clears
  `a`, several moves) and, near its end, itself executes `bset #$0,y:$6` -- **setting the very bit that gates
  this branch** -- before falling through and rejoining at `$0001d1`, the same place int jumped to directly.

**Interpretation: `Y:$6` bit 0 is a "this setup already ran" flag.** One build believes it's already been
done (skips the subroutine); the other doesn't (runs it, then marks it done). Both converge back to the same
PC (`$1d1`) but with genuinely different register state, because one build executed real extra code
(`func_00025b`) the other legitimately skipped as already-done. This is the actual root of the divergence
found across three sessions now -- not a DO-loop/SR_LF bug (that was a downstream symptom: the two builds'
subsequent control flow differs enough, after this fork, that they eventually enter different loops too),
not a Timer2/TCSR2 bug (that was a red herring from mis-correlating trace granularities), not a 68k-timing
bug (also a downstream symptom).

**Not yet found: why `Y:$6` bit 0 differs at this point.** Two theories checked, one ruled out:

- *Interrupt-timing theory (ruled out for this occurrence):* the only non-`bset` write to `Y:$6` found in the
  disassembled range is `move n6,y:$6` at `$000083`, which sits in the DSP's exception-vector address range
  (`$00`-~`$100`) -- looked like it could be an ISR whose firing point (only checked once per
  `execInterpreter()`/block call, via `m_interruptFunc`, same granularity concern as `execPeriph()`) could
  shift relative to surrounding code between interpreted and block-batched dispatch. Checked directly: **zero
  occurrences of PC `$80`-`$84` anywhere in a 20,000-record ring spanning the fork point, in either build** --
  this code path simply isn't running nearby, so it isn't the mechanism here.

- *Re-read `func_00025b` itself* (the subroutine gated by the `Y:$6` bit 0 check): it is **not** idempotent
  "run-once setup" as first assumed. It reads through a data table via `(r0)+` auto-increment and dispatches
  on flag bits pulled from each entry (`jset #$c,x1,...`, `jclr #$8,x1,...` etc.) -- the classic shape of "pop
  and process the next item from a queue" (most likely a pending parameter-change/MIDI-event queue for this
  synth engine), not a one-time initialization routine. That reframes what `Y:$6` bit 0 actually is: more
  likely a "there's a queued item, and it's been serviced this pass" latch than a "boot setup done" flag.

**Refined interpretation:** the fork is very likely a genuine **scheduling/timing variance in exactly when a
queued event gets serviced relative to other DSP work**, not a wrong-computation bug in any single opcode.
An interpreter (strictly one instruction at a time, checking everything at maximum granularity) and a
block-batched recompiler (checking interrupts/coarser conditions only between fused blocks) can legitimately
service an asynchronous or queued event at a different *relative* point in the instruction stream while both
remaining individually "correct" -- and once one build services it a few instructions earlier or later than
the other, real register state (here, whatever `func_00025b` computes from the queue entry) will differ from
that point on, which is exactly what was observed. This is a harder class of problem than a decode/miswrite
bug: achieving true bit-exactness would mean matching event-service timing at instruction granularity between
a per-instruction interpreter and a block-based recompiler, which cuts against the recompiler's entire
performance rationale.

**Practical next steps, in order of effort:** (1) determine what's actually enqueued/processed by
`func_00025b` (inspect the data `r0` points to at the fork -- likely MIDI or a parameter change) to judge
real-world audible impact: a several-instruction-early/late parameter update is a very different severity
than a wrong sample. (2) If it matters audibly, the fix is architectural (make event/interrupt servicing
granularity match the interpreter's, e.g. by forcing a block boundary wherever a pending event could be
serviced) rather than a small local patch. (3) If it does *not* matter audibly -- plausible, since the first
730 samples matched perfectly and this is boot-adjacent housekeeping, not the audio hot path -- Vavra's
recompiler may already be good enough for real-world use despite this known, narrow, now well-understood gap
from strict bit-exactness.

## Confirmed: it's an event-scheduling timing skew, not a data bug (2026-09-30, later still)

**Did step (1) above.** Added `GM_QDUMP` (env-gated, `gm_probe.cpp`): whenever `func_00025b` is entered,
dumps `r0` and 12 words of X: memory from there. `r0` is constant (`$0011a0`) every single call -- it's not
a moving queue pointer, it's a **fixed event-record slot**, re-read each time. The first word there is a
monotonically-incrementing counter (`...901, 902, 903, 800 (wrapped), 801, 802...`), with the following words
looking like payload (`0x80063e`/`0x005737`/`0x000000`/`0x7fffff`-pattern data -- plausibly a MIDI note/CC
event's parameters, `0x7fffff` being a common "max"/sentinel value for a 24-bit fixed-point field).

**Confirmed directly, with concrete numbers, that this is a timing skew, not lost/duplicated/corrupted
events.** Both builds process the exact same sequence of counter values in the same order -- no event is
skipped or repeated. But right at the fork: **the recompiled build processes counter `$802` at cycle
319,548,603, about 50 cycles *before* the previously-established fork point (~319,548,555-319,549,090)**;
the interpreter's last captured call in the same window was still processing the *previous* counter value
(`$801`) some 43,000 cycles earlier (cycle 319,505,356) and hadn't yet serviced `$802` by the time the ring
capture ended. In other words: **the recompiled build simply notices and services a newly-arrived
event a little sooner, in relative DSP-cycle terms, than the interpreter does** -- exactly the "event
serviced at a different relative point in the instruction stream" mechanism theorized above, now backed by
the actual event data rather than just control-flow shape.

**Practical conclusion:** given the counter/payload words look like a single MIDI note or CC event (not
audio-sample data), and the skew is on the order of tens to low hundreds of DSP cycles (microseconds, well
below a single audio sample period), this is very likely an **inaudible, sub-sample timing jitter in exactly
when a MIDI event's parameters get applied** -- not corrupted audio and not a functional bug in the usual
sense. Combined with the earlier finding that the first 730 output samples matched bit-for-bit, the practical
read is: **Vavra's recompiler is very likely usable for real-world listening despite this known, now fully
understood gap from strict bit-exactness.** Actually eliminating the gap would mean matching MIDI/event
servicing granularity to the interpreter's at the block-dispatch level, which is an architectural change (and
in tension with the whole performance rationale for block-based recompilation) -- not recommended unless a
concrete audible artifact is found that traces back to this.

**If this needs to be revisited:** the two clean next diagnostics are (a) decode the event payload fully
against this ROM's actual MIDI/event-record format (need the mqLib source's parameter-event struct layout) to
confirm it's a note/CC and not something more consequential like a patch-load or voice-allocation event, and
(b) extend `GM_QDUMP` to also log whichever code *sets* the incrementing counter (the uC-to-DSP event
delivery path, likely via HDI08) to see exactly how the delivery timing itself differs between builds --
though per the earlier uC-side interleaved trace work, the 68k's own instruction/cycle stream was shown to
match the interpreter's for a very long stretch, so the skew's origin is more likely inside the DSP-side
scheduling (block dispatch checking for events less granularly) than the uC side re-introducing timing drift.

**Cross-checked against `mpc-vst-machinedrum`'s own gearmulator-md-mm fork** (a separate, VE-enhanced fork the
user maintains for Machinedrum/Monomachine work), specifically `doc/vavra_performance_regression_223.md`.
Its source comment for the exact same protocol reads: `// BatchComplete: move n6,y:$6 => Y:$6 = N6 (bit 0 = 0
= pending)` -- an independent, canonical confirmation that `Y:$6` bit 0 is exactly the documented HDI08
BatchStart/BatchComplete command-processing flag this session reverse-engineered from the disassembly alone.
That doc also lists specific performance regressions in the VE fork (HDI08 RX rate limit 0->100, a mutex on
every HDI08 word transfer, a TXDE back-pressure wait, batch-sync `ucYieldLoop` calls) -- checked our upstream
`dsp56300/gearmulator` checkout directly: none of these are present (`setRXRateLimit(0)`, no mutex), so this
codebase doesn't carry that fork's regressions.

## Performance: confirmed ~4.75x real time, and it's genuinely that slow (2026-09-30, later still)

**Re-measured Vavra's recompiled build on the Force directly** (armhf cross-build, same flags as the earlier
"fixed" build: `NO_JIT_RUNTIME, EXEC_STATS, INTERP_DEFAULT, INTERP_CYCLES, SPIN_SKIP, GM_LOCKSTEP, RECOMP`,
`-I recomp-mq`, `taskset -c 3`). First attempt showed alarming, seemingly-growing slowdown with run length
(10s audio: 4.53-5.16x; 30s: 4.86x; 60s: 7.70x, with the last 30s segment alone implying ~10.5x) and CPU
frequency/thermal checks (`scaling_cur_freq` pinned at 1.8GHz throughout, temp 67-69C, `performance` governor)
ruled out thermal throttling as the cause.

**Root cause of the noisy numbers: the user was using Machinedrum on the same physical device at the time.**
Once confirmed quiet, five independent 10-second runs came back at **4.73-4.75x, essentially zero variance**
-- matching the previously-recorded 4.75x almost exactly. So the apparent quadratic slowdown was real-world
background interference on shared, live hardware (exactly what the Monomodule Stage-3 benchmarking notes
warned about: "don't benchmark without pacing/isolation, a live device has other processes"), not a bug in
Vavra's own code. Good to have ruled out, but it also means: **this is the real, honest, steady-state number
for the current codebase. There is no hidden quick win here** -- the earlier hope that a "Stage 3 pass" would
improve it doesn't apply, because Stage 3's wins are in the *shared* DSP56300 core (dead-CCR elimination,
`alu_mpy`, AGU fast paths, HDI08 TX polling, etc. -- all already in this fork's git history and used by every
synth built from it, Vavra included). **Vavra's 4.75x already includes every one of those optimizations.**
Getting real-time would need genuinely new, Vavra/mQ-specific optimization work (its own hot-path profile,
its own interpreter-fallback coverage, its own peripheral-emulation cost), not re-running work already done
for Monomodule.

**Practical assessment for "is a port feasible":** correctness looks solid (one narrow, likely-inaudible
timing gap, fully understood). Performance does not: ~4.75x means roughly 5x too slow for real-time on this
hardware, and that's already with the best shared-core optimizations applied. A real-time Vavra port would
need a dedicated optimization pass at least as involved as Monomodule's Stage 3 was, targeted at this
specific ROM's hot paths (no profiler tooling for this was set up this session -- no `perf` binary was
available on the Force and building/deploying a static one is the natural next step if this is pursued).

## Started the optimization pass: got `perf` on the Force, found the real bottleneck (2026-09-30, later still)

**Got on-device profiling working.** No `perf` binary exists on the Force and none was bundled from earlier
work. Built one: `dpkg --add-architecture armhf` inside `mnm-armhf-builder`, `apt-get install
--download-only linux-perf:armhf` (pulls the full armhf dependency chain, 63 packages), extracted every
`.so` from those debs into a bundle deployed alongside a `-g` build. **Caution for next time:** the first
attempt set `LD_LIBRARY_PATH` for the whole SSH session including core glibc libs (`libc.so.6`, `libm.so.6`,
`ld-linux-armhf.so.3`) bundled from a mismatched Debian version -- this broke basic system tools (`head`
failed with a GLIBC version error) for the duration of that one shell. No lasting damage (scoped to that SSH
invocation only), but the fix is to **exclude core glibc/loader libs from the bundle** (they're always
already on the device) and only ever export `LD_LIBRARY_PATH` for the single `perf` invocation, never as a
persistent session export. The trimmed bundle plus `perf record -F 4000` worked cleanly.

**Result: 14.77% of all samples are in `DSP::do_exec` alone, and the full uC/DSP lockstep scheduling
machinery (`lockstepStepDsp`, `processUcCycle`, `MqDsp::lockstepStep`, `Hardware::lockstepRun`,
`dspExecPeripherals`, `m68k_execute`, the uC's own peripheral `exec()`s) sums to roughly 40% of total
samples** -- dwarfing every individual recompiled block (each under ~1.2% individually; hundreds of them
exist). This pointed at `GM_LOCKSTEP` itself: it was added purely as a **correctness-testing harness**
(deterministic single-thread execution so recompiled output can be hashed against the interpreter's -- see
"Vavra lock-step + recompiler attempt" above) -- never intended as the production design. Its comment
("One exec() call per step, not a fixed batch... batching would let the DSP run far past HDI08 round-trips")
confirms it deliberately serializes at maximum granularity for safety during boot's HDI08 handshake, and
this has been running for every measurement all session, including the "confirmed ~4.75x" figure above.

**Rebuilt without `GM_LOCKSTEP`** (normal gearmulator threading: real uC and DSP threads, natural HDI08
buffering) and re-measured on the Force: **3.72x** -- a real, ~22% improvement, but far short of what the
scheduling overhead's ~40% profile share might have suggested. The per-thread CPU breakdown explains why:
for 10 audio-seconds, the `DSP A` thread alone consumed **28.70 CPU-seconds (2.87x) in complete isolation on
its own thread**, `MC68331` (uC) 8.13s. **This is the real, scheduling-independent bottleneck: the recompiled
DSP code itself needs to run about 2.87x faster than it currently does.** Removing the lockstep-induced
scheduling overhead was worth doing (and is a legitimate, real win for any eventual production build) but it
was never going to close a 4.75x gap on its own -- most of that gap is genuine DSP-side execution cost, the
same category of problem Monomodule's Stage 3 solved for its own synth.

**This is consistent with, not contradictory to, the earlier "Stage 3 is already applied" finding.** Stage
3's wins are generic, shared-core optimizations (dead-CCR elimination, `alu_mpy`, AGU fast paths) that
benefit any synth built from this fork equally -- Vavra already has them. What Vavra hasn't had is a
profiling pass *of its own hot paths*, the same kind of iterative "measure, optimize the top offender,
re-measure" work Stage 3's table documents for Monomodule. The `perf` data above is exactly the input that
process needs; the individual `recompBlock<N>`/`recompLoop<N>` entries in the full report (`perf_report.txt`
in the session's scratch area, not committed -- regenerable via the steps above) are where to start.

**Next concrete steps:** (1) build the non-`GM_LOCKSTEP` threaded configuration into the standard
benchmark path (it's a real, keepable win) alongside continuing to use `GM_LOCKSTEP` only for correctness
verification, never performance measurement, going forward. (2) Use `addr2line`/disassembly on the hottest
individual `recompBlock<N>` functions from the `perf` data to find what DSP56300 opcode patterns dominate
Vavra's actual voice/filter code (distinct from Monomodule's), and look for Stage-3-style opportunities
specific to them (dead-CCR variants not yet covered, parallel-move latch overhead, etc.). (3) Investigate
whether `do_exec`'s remaining share (still present, just smaller, without `GM_LOCKSTEP`) has its own
avoidable overhead independent of the lockstep scheduler.
