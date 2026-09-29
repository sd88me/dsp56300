// Gearmulator-on-Force feasibility probe (arm32 study). One binary per synth, chosen by GM_SYNTH_*.
// usage: gm_probe <romdir> [seconds=20] [dump.raw]
// Runs a scripted workload in lock step (block-granular MIDI), prints an FNV hash of every output
// sample plus the interpreter's executed-instruction count and wall time.
#include <unistd.h>
#include <dirent.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <thread>

#include "synthLib/device.h"
#include "synthLib/romLoader.h"
#include "baseLib/logging.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/opcodes.h"
#include "mc68k/mc68k.h"

#if defined(GM_SYNTH_VIRUS)
#include "virusLib/device.h"
#include "virusLib/romloader.h"
#elif defined(GM_SYNTH_MQ)
#include "mqLib/device.h"
#include "mqLib/romloader.h"
#elif defined(GM_SYNTH_XT)
#include "xtLib/xtDevice.h"
#include "xtLib/xtRomLoader.h"
#elif defined(GM_SYNTH_N2X)
#include "n2xLib/n2xdevice.h"
#include "n2xLib/n2xromloader.h"
#endif

#if defined(GM_SYNTH_MQ) && defined(GM_LOCKSTEP)
namespace mqLib { extern uint64_t g_lsProfDspNs, g_lsProfUcNs, g_lsProfUcCycles; }
#endif

#ifdef GM_DISCOVER
// ---- recompiler discovery (see tools/arm32jit_prototype/recomp/mnm_recomp_discover.cpp for the format) ----
#include <map>
#include <set>
#include <tuple>
#include <dlfcn.h>
#include "dsp56kEmu/opcodes.h"
#include "dsp56kEmu/opcodeanalysis.h"
#include "dsp56kEmu/opcodecycles.h"
namespace disc {
using namespace dsp56k;
std::map<TWord, uint64_t> g_runCount;
std::map<TWord, std::pair<TWord, TWord>> g_firstWords;
uint64_t g_conflicts = 0;
std::map<TWord, uint64_t> g_conflictPcs;
std::set<TWord> g_entries, g_loopEnds;
TWord g_expectedNext = 0xffffffff;
Opcodes& ops() { static Opcodes instance; return instance; }	// lazy: avoid static-init-order issues with a namespace-scope global
TWord lengthAt(DSP* d, TWord pc) {
	const TWord a = d->memory().get(MemArea_P, pc);
	Instruction ia = Nop, ib = Invalid;
	if (a) ops().getInstructionTypes(a, ia, ib);
	const auto len = Opcodes::getOpcodeLength(a, ia, ib);
	return len ? len : 1;
}
void hook(DSP* d, TWord pc) {
	++g_runCount[pc];
	const TWord a = d->memory().get(MemArea_P, pc), b = d->memory().get(MemArea_P, pc + 1);
	auto it = g_firstWords.find(pc);
	if (it == g_firstWords.end()) g_firstWords[pc] = {a, b};
	else if (it->second.first != a) { ++g_conflicts; ++g_conflictPcs[pc]; }
	if (pc != g_expectedNext) g_entries.insert(pc);
	if (d->regs().sr.var & SR_LF) g_loopEnds.insert(TWord(d->regs().la.var));
	g_expectedNext = pc + lengthAt(d, pc);
}
uintptr_t off(void* p) { Dl_info i{}; return p && dladdr(p, &i) ? uintptr_t(p) - uintptr_t(i.dli_fbase) : 0; }
void write(const char* path) {
	DSP& d = *DSP::firstRegistered();
	FILE* f = std::fopen(path, "w");
	for (const auto& [pc, n] : g_runCount) {
		const TWord a = d.memory().get(MemArea_P, pc), b = d.memory().get(MemArea_P, pc + 1);	// final contents, consistent with the resolved handlers
		Instruction ia = Nop, ib = Invalid;
		if (a) ops().getInstructionTypes(a, ia, ib);
		const TWord len = Opcodes::getOpcodeLength(a, ia, ib);
		if (pc == 0x0013b1) {
			fprintf(stderr, "DEBUG pc=0013b1 a=%06x b=%06x ia=%d ib=%d len=%u isNonParallel(word)=%d\n", a, b, (int)ia, (int)ib, len, (int)Opcodes::isNonParallelOpcode(a));
			for (size_t k = 0; k < 400; ++k) {
				const OpcodeInfo& oik = Opcodes::getOpcodeInfoAt(k);
				const auto mk = oik.m_mask1 | oik.m_mask0;
				if ((a & mk) == oik.m_mask1)
					fprintf(stderr, "  DEBUG raw match inst=%zu mask0=%06x mask1=%06x\n", k, oik.m_mask0, oik.m_mask1);
			}
		}
		const auto ri = d.getRecompInfo(pc);
		const auto flags = Opcodes::getFlags(ia, ib);
		RegisterMask written = RegisterMask::None, read = RegisterMask::None;
		Opcodes::getRegisters(written, read, a, ia, ib);
		constexpr auto ctrl = RegisterMask::PC | RegisterMask::LA | RegisterMask::LC | RegisterMask::SSH | RegisterMask::SSL |
		                      RegisterMask::SP | RegisterMask::SC | RegisterMask::EP | RegisterMask::SZ | RegisterMask::EMR |
		                      RegisterMask::MR | RegisterMask::OMR;
		int kind = 0;
		auto isPoll = [](Instruction i) { return i == Jset_pp || i == Jclr_pp || i == Jset_qq || i == Jclr_qq || i == Brset_pp || i == Brclr_pp || i == Brset_qq || i == Brclr_qq; };
		if (!ri.resolved || (flags & (OpFlagLoop | OpFlagRepDynamic | OpFlagRepImmediate)) || ia == Wait || ia == Ifcc ||
		    ia == Ifcc_U || ib == Ifcc || ib == Ifcc_U || isPoll(ia) || isPoll(ib))
			kind = 2;
		else if ((flags & (OpFlagBranch | OpFlagPopPC)) || (written & ctrl) != RegisterMask::None)
			kind = 1;
		bool moveAB = true;
		uint64_t pmr = 0, pmw = 0, par_ = 0, paw = 0;
		if (ri.parallel) {
			RegisterMask mw = RegisterMask::None, mr = RegisterMask::None, aw = RegisterMask::None, ar = RegisterMask::None;
			Opcodes::getRegisters(mw, mr, a, ib, Invalid);
			Opcodes::getRegisters(aw, ar, a, ia, Invalid);
			auto touches = [](RegisterMask m, RegisterMask acc) { return (m & acc) != RegisterMask::None; };
			moveAB = (touches(aw, RegisterMask::A) && touches(mw | mr, RegisterMask::A)) ||
			         (touches(aw, RegisterMask::B) && touches(mw | mr, RegisterMask::B));
			pmr = uint64_t(mr); pmw = uint64_t(mw); par_ = uint64_t(ar); paw = uint64_t(aw);
		}
		const bool readsPC = (read & RegisterMask::PC) != RegisterMask::None;
		const int ccr = int((read & RegisterMask::CCR) != RegisterMask::None) |
		                int((written & RegisterMask::CCR) != RegisterMask::None || (flags & OpFlagCCR)) << 1 |
		                int((flags & OpFlagCondition) != 0) << 2;
		const uint32_t cy = a ? calcCycles(ia, ib, pc, a, 0, 1) : 1;
		std::fprintf(f, "I %06x %06x %06x %u %d %d %zx %zx %zx %llu %d %d %d %llx %llx %llx %llx %u\n", pc, a, b, len ? len : 1,
		             kind, int(ri.parallel), off(ri.op), off(ri.opMove), off(ri.opAlu), (unsigned long long)n, int(moveAB), int(readsPC), ccr,
		             (unsigned long long)pmr, (unsigned long long)pmw, (unsigned long long)par_, (unsigned long long)paw, cy);
	}
	for (auto e : g_entries) std::fprintf(f, "E %06x\n", e);
	for (auto l : g_loopEnds) std::fprintf(f, "L %06x\n", l);
	std::fclose(f);
	for (const auto& [pc, n] : g_conflictPcs) if (n > 100000) std::fprintf(stderr, "  conflicting pc %06x: %llu executions of a different word\n", pc, (unsigned long long)n);
	std::fprintf(stderr, "discovery: %zu distinct pcs, %llu word conflicts (address held different code)\n", g_runCount.size(), (unsigned long long)g_conflicts);
}
}
#endif

#ifdef DSP56K_RECOMP_DISCOVERY
// Differential comparator: log a compact per-instruction register trace so two builds (interpreter vs
// recompiled) can be diffed index-for-index to find the exact first point of divergence. Independent of
// GM_DISCOVER (the block-building harness) -- this just needs the trace hook, which fires once per real
// instruction as long as blocks are MAX_INSTR=1 on the recompiled side (see recomp_gen_gm.py).
namespace regtrace {
FILE* g_file = nullptr;
uint64_t g_count = 0, g_cap = 0;
#pragma pack(push, 1)
// kind 0 = DSP instruction, kind 1 = uC (68k) instruction. Both hooks write into the same file, in true
// call order, so the interleaving itself (not just each side's own counters) can be diffed between builds --
// this is what's needed to catch a scheduling/ordering divergence that leaves each side's own counters intact.
struct Rec { uint8_t kind; uint8_t pad[7]; uint32_t pc; uint64_t a; uint64_t b; uint32_t sr; uint64_t cycles; uint64_t instr; };
#pragma pack(pop)
void hookDsp(dsp56k::DSP* d, dsp56k::TWord pc) {
	if (g_count >= g_cap) { std::fclose(g_file); fprintf(stderr, "regtrace: cap reached, wrote %llu records\n", (unsigned long long)g_count); _exit(0); }
	Rec r{}; r.kind = 0; r.pc = pc; r.a = d->regs().a.var; r.b = d->regs().b.var; r.sr = d->regs().sr.var; r.cycles = d->getCycles(); r.instr = d->getInstructionCounter();
	std::fwrite(&r, sizeof(r), 1, g_file);
	++g_count;
	if ((g_count & 0xffff) == 0) std::fflush(g_file);	// survives a kill -9 if the workload never naturally ends
}
void hookUc(mc68k::Mc68k* u, uint32_t pc, uint64_t cycles) {
	if (g_count >= g_cap) { std::fclose(g_file); fprintf(stderr, "regtrace: cap reached, wrote %llu records\n", (unsigned long long)g_count); _exit(0); }
	Rec r{}; r.kind = 1; r.pc = pc; r.cycles = cycles;
	std::fwrite(&r, sizeof(r), 1, g_file);
	++g_count;
	if ((g_count & 0xffff) == 0) std::fflush(g_file);
}
}
#endif
using namespace synthLib;
using clk = std::chrono::steady_clock;

static uint64_t g_hash = 1469598103934665603ull;
static void hashWord(uint32_t w) { g_hash ^= w; g_hash *= 1099511628211ull; }

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : ".";
	const double seconds = argc > 2 ? atof(argv[2]) : 20.0;
	FILE* dump = argc > 3 ? fopen(argv[3], "wb") : nullptr;

#ifdef GM_DISCOVER
	dsp56k::DSP::s_recompTraceHook = &disc::hook;
#endif
	if (!getenv("GM_LOG")) Logging::setLogFunc([](const std::string&) {});	// this device's LOG() macro is unconditional and dominates runtime otherwise
#ifdef DSP56K_RECOMP_DISCOVERY
	if (const char* rt = getenv("GM_REGTRACE")) {
		regtrace::g_file = std::fopen(rt, "wb");
		regtrace::g_cap = getenv("GM_REGTRACE_CAP") ? strtoull(getenv("GM_REGTRACE_CAP"), nullptr, 10) : 3000000ull;
		dsp56k::DSP::s_recompTraceHook = &regtrace::hookDsp;
		if (getenv("GM_REGTRACE_UC")) mc68k::Mc68k::s_traceHook = &regtrace::hookUc;
	}
#endif
	RomLoader::setSearchPath(dir);
	DeviceCreateParams p;
#if defined(GM_SYNTH_VIRUS)
	auto rom = virusLib::ROMLoader::findROM();
	if (!rom.isValid()) { puts("no virus rom"); return 2; }
	p.romName = rom.getFilename(); p.romData = rom.getRomFileData(); p.customData = static_cast<uint32_t>(rom.getModel());
	std::unique_ptr<Device> dev(new virusLib::Device(p));
#elif defined(GM_SYNTH_MQ)
	auto rom = mqLib::RomLoader::findROM();
	if (!rom.isValid()) { puts("no mq rom"); return 2; }
	p.romData = rom.getData(); p.romName = rom.getFilename();
	std::unique_ptr<Device> dev(new mqLib::Device(p));
#elif defined(GM_SYNTH_XT)
	auto rom = xt::RomLoader::findROM();
	if (!rom.isValid()) { puts("no xt rom"); return 2; }
	p.romData = rom.getData(); p.romName = rom.getFilename();
	std::unique_ptr<Device> dev(new xt::Device(p));
#elif defined(GM_SYNTH_N2X)
	auto rom = n2x::RomLoader::findROM();
	if (!rom.isValid()) { puts("no n2x rom"); return 2; }
	p.romData.assign(rom.data().begin(), rom.data().end()); p.romName = rom.getFilename();
	std::unique_ptr<Device> dev(new n2x::Device(p));
#endif
	if (!dev->isValid()) { puts("device invalid"); return 3; }

	const float sr = dev->getSamplerate();
	const uint32_t nOut = dev->getChannelCountOut();
	const size_t block = 64;
	std::vector<std::vector<float>> outBuf(12, std::vector<float>(block)), inBuf(4, std::vector<float>(block, 0.0f));
	TAudioInputs ins{}; TAudioOutputs outs{};
	for (size_t i = 0; i < 4; ++i) ins[i] = inBuf[i].data();
	for (size_t i = 0; i < 12; ++i) outs[i] = outBuf[i].data();

	std::vector<SMidiEvent> midiIn, midiOut;
	auto ev = [&](uint8_t a, uint8_t b, uint8_t c) { midiIn.emplace_back(MidiEventSource::Host, a, b, c, 0u); };

	const bool wide = getenv("GM_WIDE") != nullptr;
	const uint64_t blocks = static_cast<uint64_t>(seconds * sr / block);
	// the script: every 1.0 s a new program + chord + CC sweep, chord released after 0.7 s
	const uint64_t period = static_cast<uint64_t>(sr / block);
	static const uint8_t chord[] = {36, 48, 55, 60, 64, 67, 72, 79};
	static const uint8_t ccs[] = {1, 74, 71, 73, 72, 91, 93, 5, 7, 10};

#if defined(GM_SYNTH_MQ) && defined(GM_LOCKSTEP)
	mqLib::g_lsProfDspNs = mqLib::g_lsProfUcNs = mqLib::g_lsProfUcCycles = 0;	// profile the workload, not the boot
#endif
	const uint64_t i0 = dsp56k::DSP::execCountAll();
	const auto t0 = clk::now();
	for (uint64_t b = 0; b < blocks; ++b)
	{
		midiIn.clear(); midiOut.clear();
		const uint64_t step = b / period, ph = b % period;
		if (ph == 0)
		{
			ev(0xb0, 0, static_cast<uint8_t>((step / 32) & 3));       // bank
			ev(0xc0, static_cast<uint8_t>((step * 7) & 127), 0);      // program
			for (int k = 0; k < 4 + static_cast<int>(step % 5); ++k)
				ev(0x90, chord[(k + step) % 8], static_cast<uint8_t>(60 + ((step * 13 + k * 17) % 60)));
		}
		if (ph > 4 && ph % 8 == 0)
		{
			const uint8_t cc = ccs[(step + ph / 8) % 10];
			ev(0xb0, cc, static_cast<uint8_t>(((ph * 3 + step * 29) * 5) & 127));
		}
		if (wide)
		{
			static uint32_t rng = 12345;
			auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
			if (ph > 0 && ph % (period / 24) == 0)
			{
				SMidiEvent e(MidiEventSource::Host);
				const uint32_t idx = rnd() % 392;
				e.sysex = {0xf0, 0x3e, 0x10, 0x7f, 0x20, 0x20, static_cast<uint8_t>(idx >> 7), static_cast<uint8_t>(idx & 0x7f), static_cast<uint8_t>(rnd() & 0x7f), 0xf7};
				midiIn.push_back(e);
			}
			if (ph > 0 && ph % (period / 12) == 3)
			{
				ev(0xb0, static_cast<uint8_t>(1 + rnd() % 119), static_cast<uint8_t>(rnd() & 127));
				ev(0xe0, static_cast<uint8_t>(rnd() & 127), static_cast<uint8_t>(rnd() & 127));
				ev(0xd0, static_cast<uint8_t>(rnd() & 127), 0);
			}
			if (ph % 40 == 0 && (step % 3) != 0) ev(0xf8, 0, 0);
			if (ph == 5 && (step % 6) == 1) ev(0xfa, 0, 0);
			if (ph == 5 && (step % 6) == 4) ev(0xfc, 0, 0);
			if (ph == 7 && (step % 4) == 2) ev(0xb0, 64, 127);
			if (ph == period * 9 / 10 && (step % 4) == 2) ev(0xb0, 64, 0);
		}
		if (ph == period * 7 / 10)
			for (int k = 0; k < 8; ++k) ev(0x80, chord[k], 0);
		dev->process(ins, outs, block, midiIn, midiOut);
		if (getenv("GM_PROGRESS") && (b & 0x3ff) == 0)
			fprintf(stderr, "progress: block=%llu/%llu instr=%llu\n", (unsigned long long)b, (unsigned long long)blocks, (unsigned long long)dsp56k::DSP::execCountAll());
		for (size_t s = 0; s < block; ++s)
			for (uint32_t c = 0; c < nOut && c < 12; ++c)
			{
				uint32_t w; memcpy(&w, &outBuf[c][s], 4); hashWord(w);
				if (dump) fwrite(&outBuf[c][s], 4, 1, dump);
			}
	}
	const double wall = std::chrono::duration<double>(clk::now() - t0).count();
	const uint64_t instr = dsp56k::DSP::execCountAll() - i0;
	printf("synth=%s sr=%.0f out=%u audio=%.2fs wall=%.2fs rt=%.2fx exec_instr=%llu rate=%.2f Minstr/s hash=%016llx\n",
		GM_NAME, sr, nOut, blocks * block / sr, wall, wall / (blocks * block / sr),
		(unsigned long long)instr, instr / (blocks * block / sr) / 1e6, (unsigned long long)g_hash);
	{	// per-thread CPU seconds (whole run incl. boot)
		DIR* d = opendir("/proc/self/task");
		while (auto* e = d ? readdir(d) : nullptr)
		{
			if (e->d_name[0] == '.') continue;
			char path[128]; snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
			FILE* f = fopen(path, "r"); if (!f) continue;
			char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
			char* cl = strrchr(buf, ')'); char* op = strchr(buf, '(');
			if (!cl || !op) continue;
			*cl = 0; unsigned long ut = 0, st = 0;
			sscanf(cl + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
			printf("thread %-16s cpu=%.2fs\n", op + 1, (ut + st) / (double)sysconf(_SC_CLK_TCK));
		}
		if (d) closedir(d);
	}
#if defined(GM_SYNTH_MQ) && defined(GM_LOCKSTEP)
	if (getenv("GM_LSPROF"))
		printf("lockstep profile: dsp=%.2fs uc=%.2fs (uc cycles %.1fM = %.1f Mcycles/s of uc time)\n", mqLib::g_lsProfDspNs / 1e9, mqLib::g_lsProfUcNs / 1e9, mqLib::g_lsProfUcCycles / 1e6, mqLib::g_lsProfUcCycles / 1e6 / (mqLib::g_lsProfUcNs / 1e9));
#endif
	printf("spin_skipped=%llu\n", (unsigned long long)dsp56k::DSP::spinSkippedAll());
#ifdef GM_DISCOVER
	if (getenv("GM_TRACE")) disc::write(getenv("GM_TRACE"));
#endif
#ifdef DSP56K_RECOMP_DISCOVERY
	if (regtrace::g_file) { std::fclose(regtrace::g_file); fprintf(stderr, "regtrace: wrote %llu records\n", (unsigned long long)regtrace::g_count); }
#endif
	if (getenv("GM_HOT")) dsp56k::DSP::dumpHotAll(static_cast<size_t>(atoi(getenv("GM_HOT"))));
	if (const char* da = getenv("GM_DISASM")) {
		// one-off diagnostic: disassemble a window of P memory around a given address, using the ROM's
		// final, resolved contents at process end (after boot has downloaded the real program).
		uint32_t center = strtoul(da, nullptr, 16);
		uint32_t before = getenv("GM_DISASM_BEFORE") ? strtoul(getenv("GM_DISASM_BEFORE"), nullptr, 10) : 10;
		uint32_t after  = getenv("GM_DISASM_AFTER")  ? strtoul(getenv("GM_DISASM_AFTER"), nullptr, 10) : 10;
		auto* d = dsp56k::DSP::firstRegistered();
		if (d) {
			dsp56k::Opcodes opcodes;
			dsp56k::Disassembler disasm(opcodes);
			uint32_t pc = center > before ? center - before : 0;
			const uint32_t end = center + after;
			while (pc <= end) {
				const dsp56k::TWord a = d->memory().get(dsp56k::MemArea_P, pc);
				const dsp56k::TWord b = d->memory().get(dsp56k::MemArea_P, pc + 1);
				std::string text;
				const uint32_t len = disasm.disassemble(text, a, b, d->regs().sr.var, d->regs().omr.var, pc);
				printf("%s%06x: %06x %06x  %s\n", pc == center ? "-> " : "   ", pc, a, b, text.c_str());
				pc += len ? len : 1;
			}
		} else {
			fprintf(stderr, "GM_DISASM: no registered DSP instance\n");
		}
	}
	if (dump) fclose(dump);
	fflush(stdout);
	_exit(0);	// destructors can hang on a DSP that never reaches WAIT (interpreter build)
}
