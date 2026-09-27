# Motorola DSP 56300 family emulator

[![CMake][s0]][l0] ![GPLv3][s1]

[s0]: https://github.com/dsp56300/dsp56300/actions/workflows/cmake.yml/badge.svg
[l0]: https://github.com/dsp56300/dsp56300/actions/workflows/cmake.yml

[s1]: https://img.shields.io/badge/license-GPLv3-blue.svg

## This fork (`sd88me/dsp56300`, branch `arm32`)

Forked from [dsp56300/dsp56300](https://github.com/dsp56300/dsp56300) to add a **static recompiler for
32-bit ARM**, where there is no runtime JIT backend (upstream's JIT supports x86-64 and 64-bit ARM
only). Built for [Monomodule for MPC OS](https://github.com/sd88me/mpc-vst-monomodule) (an Elektron
Monomachine emulation running on the Akai Force's Cortex-A17), and independently useful to any
dsp56300-based product targeting 32-bit ARM.

**What it does:** the interpreter's own opcode handlers get called from generated C++ with the opcode
baked in as a compile-time constant, so the compiler can inline and constant-fold what the interpreter
can't — bit-exact with the interpreter, no new instruction semantics. `libs/dsp56300` docs:
[`docs/ARM32_JIT.md`](docs/ARM32_JIT.md) (design, every gate passed, and what didn't work) and
[`tools/arm32jit_prototype/recomp/README.md`](tools/arm32jit_prototype/recomp/README.md) (the generator
pipeline — discovery trace → basic blocks → generated `.inl`). It's in real use: see
[mpc-vst-monomodule](https://github.com/sd88me/mpc-vst-monomodule)'s README for the shipped result
(bit-exact on all 22 machines, real-time on the Force).

**Status and what's staying here vs. going upstream:** most of this fork is the recompiler itself —
sizeable, opinionated to the "no runtime JIT" case, and still gathering real-world use (see
`tools/arm32jit_prototype/` for exploratory work against other dsp56300-based synths: Vavra, Osirus,
Virus A, Xenia). That stays here as an independent branch/fork rather than a quick PR. A handful of
commits are small, self-contained and generically correct or beneficial on any platform — those are
being proposed upstream separately (see each commit message; several fix or touch
[dsp56300#8](https://github.com/dsp56300/dsp56300/issues/8), an open upstream bug this fork found and
fixed along the way). If you're using this fork for something other than 32-bit ARM, everything below
"Emulation of the..." is unmodified upstream code.

### Emulation of the Motorola/Freescale/NXP 56300 family DSP

This DSP has been used in plenty of virtual analogue synthesizers and other musical gear that was released after around the mid 90s, such as Access Virus A, B, C, TI / Clavia Nord Lead 3 / Waldorf Q, Microwave II / Novation Supernova, Nova and many others.

The emulator should compile just fine on any platform that supports C++17, no configure is needed as the code uses C++17 standard data types. For performance reasons, it makes excessive use of C++17 features, for example to parse opcode definitions at compile time and to create jump tables of template permutations, so C++17 is a strong requirement.

The build system used is [cmake](https://cmake.org/).

### Development

Please note that this project is a generic DSP emulator and outputs nothing but a static library after building. To use it, you need to create a project on your own, which can be a command line app, a VST plugin or whatever and instantiate the DSP class and feed data into it.

Minimal example:
```c++
#include "../dsp56300/source/dsp56kEmu/dsp.h"

int main(int argc, char* argv[])
{
	// Create DSP memory
	constexpr TWord g_memorySize = 0x040000;

	const DefaultMemoryValidator memoryMap;
	Memory memory(memoryMap, g_memorySize);

	// External SRAM starts at 0x20000
	memory.setExternalMemory(0x020000, true);

	// TODO: Load useful data into memory like this
	// Example: write a nop to P memory at address $100
	// memory.set(MemArea_P, 0x100, 0x000000);

	// Use 56362 peripherals: ESAI, HDI08
	Peripherals56362 periph;

	// Instantiate DSP
	DSP dsp(memory, &periph, &periph);

	// set starting address
	dsp.setPC(0x100); 

	while(true)
	{
		// run forever
		dsp.exec();
	}
}
```
