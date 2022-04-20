#include "polyhook2/Detour/ADetour.hpp"

#include <cmath>

std::optional<PLH::insts_t> PLH::Detour::calcNearestSz(const PLH::insts_t& functionInsts, const uint64_t prolOvrwStartOffset,
													   uint64_t& prolOvrwEndOffset) {

	uint64_t     prolLen = 0;
	PLH::insts_t instructionsInRange;

	// count instructions until at least length needed or func end
	bool endHit = false;
	for (auto inst : functionInsts) {
		prolLen += inst.size();
		instructionsInRange.push_back(inst);

		// only safe to overwrite pad bytes once end is hit
		if (endHit && !m_disasm.isPadBytes(inst))
			break;

		if (m_disasm.isFuncEnd(inst))
			endHit = true;

		if (prolLen >= prolOvrwStartOffset)
			break;
	}

	prolOvrwEndOffset = prolLen;
	if (prolLen >= prolOvrwStartOffset) {
		return instructionsInRange;
	}

	return std::nullopt;
}

bool PLH::Detour::followJmp(PLH::insts_t& functionInsts, const uint8_t curDepth, const uint8_t maxDepth) {
	if (functionInsts.size() <= 0) {
		Log::log("Couldn't decompile instructions at followed jmp", ErrorLevel::WARN);
		return false;
	}

	if (curDepth >= maxDepth) {
		Log::log("Prologue jmp resolution hit max depth, prologue too deep", ErrorLevel::WARN);
		return false;
	}

	// not a branching instruction, no resolution needed
	if (!functionInsts.front().isBranching()) {
		return true;
	}

	// might be a mem type like jmp rax, not supported
	if (!functionInsts.front().hasDisplacement()) {
		Log::log("Branching instruction without displacement encountered", ErrorLevel::WARN);
		return false;
	}

	uint64_t dest = functionInsts.front().getDestination();
	functionInsts = m_disasm.disassemble(dest, dest, dest + 100, *this);
	return followJmp(functionInsts, curDepth + 1, maxDepth); // recurse
}

void PLH::Detour::writeNop(uint64_t base, uint32_t size) {
	// we absolutely, MUST, never emit more than 8 0x90 single byte nops in a row
	/**
	https://stackoverflow.com/questions/25545470/long-multi-byte-nops-commonly-understood-macros-or-other-notation
	90                              NOP
    6690                            66 NOP
    0f1f00                          NOP DWORD ptr [EAX]
    0f1f4000                        NOP DWORD ptr [EAX + 00H]
    0f1f440000                      NOP DWORD ptr [EAX + EAX*1 + 00H]
    660f1f440000                    66 NOP DWORD ptr [EAX + EAX*1 + 00H]
    0f1f8000000000                  NOP DWORD ptr [EAX + 00000000H]
    0f1f840000000000                NOP DWORD ptr [EAX + EAX*1 + 00000000H]
    660f1f840000000000              66 NOP DWORD ptr [EAX + EAX*1 + 00000000H]
	**/
	if (size >= 2) {
		uint64_t fat = size / 2;
		bool leftOver = size % 2;
		for (uint64_t i = 0; i < fat; i++) {
			uint16_t multi_nop = 0x9066;
			mem_copy(base + i * 2, (uint64_t)&multi_nop, sizeof(multi_nop));
		}

		if (leftOver) {
			uint8_t nop = 0x90;
			mem_copy(base + fat * 2, (uint64_t)&nop, sizeof(nop));
		}
	} else if(size == 1) {
		uint8_t nop = 0x90;
		mem_copy(base, (uint64_t)&nop, sizeof(nop));
	} else {
		// this case is a nop for the nop routine :p
	}
}

bool PLH::Detour::expandProlSelfJmps(insts_t& prol,
									 const insts_t& func,
									 uint64_t& minProlSz,
									 uint64_t& roundProlSz) {
	
	uint64_t maxAddr = 0;
	const uint64_t prolStart = prol.front().getAddress();
	const branch_map_t& branchMap = m_disasm.getBranchMap();
	for (size_t i = 0; i < prol.size(); i++) {
		auto inst = prol.at(i);

		// is there a jump pointing at the current instruction?
		if (branchMap.find(inst.getAddress()) == branchMap.end())
			continue;

		insts_t srcs = branchMap.at(inst.getAddress());
		for (const auto& src : srcs) {
			const uint64_t srcEndAddr = src.getAddress() + src.size();
			if (srcEndAddr > maxAddr)
				maxAddr = srcEndAddr;
		}

		minProlSz = maxAddr - prolStart;

		// expand prol by one entry size, may fail if prol too small
		auto prolOpt = calcNearestSz(func, minProlSz, roundProlSz);
		if (!prolOpt)
			return false;
		prol = *prolOpt;
	}

	return true;
}

void PLH::Detour::buildRelocationList(
    insts_t& prologue,
    const uint64_t roundProlSz,
    const int64_t delta,
    PLH::insts_t& instsNeedingEntry,
    PLH::insts_t& instsNeedingReloc,
	PLH::insts_t& instsNeedingTranslation
) {
	assert(instsNeedingEntry.size() == 0);
	assert(instsNeedingReloc.size() == 0);
	assert(prologue.size() > 0);

	const uint64_t prolStart = prologue.front().getAddress();

	for (auto& inst : prologue) {
		if(!inst.hasDisplacement()){
			continue; // Skip instructions that don't have relative displacement
		}

		// types that change control flow
		if (inst.isBranching() &&
			(inst.getDestination() < prolStart ||
			inst.getDestination() > prolStart + roundProlSz)) {

            //indirect-call always needs an entry (only a dest-holder)
			//its destination cannot be used for relocating since it is already deferenced.(ref: inst.getDestination)
            if(inst.isCalling() && inst.isIndirect()){
				instsNeedingEntry.push_back(inst);
			}else{
				// can inst just be re-encoded or do we need a tbl entry
				const auto dispSzBits = (uint8_t)inst.getDispSize() * 8;
				const auto maxInstDisp = (uint64_t)(std::pow(2, dispSzBits - 1) - 1.0); // 2^(bitSz-1) give max val, and -1 because signed ex (int8_t [-128, 127] = [-2^7, 2^7 - 1]
				if ((uint64_t)std::llabs(delta) > maxInstDisp) {
					instsNeedingEntry.push_back(inst);
				} else {
					instsNeedingReloc.push_back(inst);
				}
			}
		}

		// data operations (duplicated because clearer)
		if (!inst.isBranching()) { // Can this happen on 32-bit?
			const auto dispSzBits = (uint8_t)inst.getDispSize() * 8;
			const auto maxInstDisp = (uint64_t)(std::pow(2, dispSzBits - 1) - 1.0);
			const auto absDelta = (uint64_t)std::llabs(delta);
            if (absDelta > maxInstDisp) {
				/*
				 * EX: 48 8d 0d 96 79 07 00    lea rcx, [rip + 0x77996]
				 * If instruction is moved beyond displacement field width
				 * we can't fix the load. Hence, we add it to the list of
				 * instructions that need to be translated to equivalent ones.
				 */
//				std::string err = "Cannot fixup IP relative data operation, needed disp. beyond max disp range: " + inst.getFullName() +
//					" needed: " + int_to_hex(absDelta) + " raw: " + int_to_hex(delta) +  " max: " + int_to_hex(maxInstDisp);
//				Log::log(err, ErrorLevel::WARN);
				instsNeedingTranslation.push_back(inst);
			}else {
				instsNeedingReloc.push_back(inst);
			}
		}
	}
}

bool PLH::Detour::unHook() {
	assert(m_hooked);
	if (!m_hooked) {
		Log::log("Detour unhook failed: no hook present", ErrorLevel::SEV);
		return false;
	}

	MemoryProtector prot(m_fnAddress, PLH::calcInstsSz(m_originalInsts), ProtFlag::R | ProtFlag::W | ProtFlag::X, *this);
	m_disasm.writeEncoding(m_originalInsts, *this);
	
	if (m_trampoline != NULL) {
		delete[](char*)m_trampoline;
		m_trampoline = NULL;
	}

	if (m_userTrampVar != NULL) {
		*m_userTrampVar = NULL;
		m_userTrampVar = NULL;
	}
	
	m_hooked = false;
	return true;
}

bool PLH::Detour::reHook()
{
	MemoryProtector prot(m_fnAddress, m_hookSize, ProtFlag::R | ProtFlag::W | ProtFlag::X, *this);
	m_disasm.writeEncoding(m_hookInsts, *this);

	// Nop the space between jmp and end of prologue
	assert(m_hookSize >= m_nopProlOffset);
	writeNop(m_fnAddress + m_nopProlOffset, m_nopSize);
	return true;
}