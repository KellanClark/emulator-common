#pragma once

#include "../types.hpp"

template <class T>
class ARM7TDMI {
public:
	T& bus;
	bool processFiq;
	bool processIrq;

	/* User Functions */
	static constexpr std::size_t BMP_BITS = 16; // Thanks to BreadFish64 because I never would've come up with this breakpoint myself
	static constexpr std::size_t TABLE_BITS = 32 - BMP_BITS;
	static constexpr std::size_t BMP_SIZE = 1 << BMP_BITS;
	static constexpr std::size_t TABLE_SIZE = 1 << TABLE_BITS;
	static constexpr std::size_t BMP_MASK = BMP_SIZE - 1;
	std::vector<std::unique_ptr<std::bitset<BMP_SIZE>>> breakpointsTable;

	ARM7TDMI(T& bus) : bus(bus), breakpointsTable(TABLE_SIZE){};

	void resetARM7TDMI()  {
		processFiq = false;
		processIrq = false;

		reg.R[0] = 0x00000000;
		reg.R[1] = 0x00000000;
		reg.R[2] = 0x00000000;
		reg.R[3] = 0x00000000;
		reg.R[4] = 0x00000000;
		reg.R[5] = 0x00000000;
		reg.R[6] = 0x00000000;
		reg.R[7] = 0x00000000;
		reg.R[8] = 0x00000000;
		reg.R[9] = 0x00000000;
		reg.R[10] = 0x00000000;
		reg.R[11] = 0x00000000;
		reg.R[12] = 0x00000000;
		reg.R[13] = 0x00000000;
		reg.R[14] = 0x00000000;
		reg.R[15] = 0x00000000;

		reg.CPSR = 0x000000D3;

		for (int i = 0; i < 8; i++) reg.R_usr[i] = 0x00000000;
		for (int i = 0; i < 8; i++) reg.R_fiq[i] = 0x00000000;
		for (int i = 0; i < 8; i++) reg.R_svc[i] = 0x00000000;
		for (int i = 0; i < 8; i++) reg.R_abt[i] = 0x00000000;
		for (int i = 0; i < 8; i++) reg.R_irq[i] = 0x00000000;
		for (int i = 0; i < 8; i++) reg.R_und[i] = 0x00000000;

		flushPipeline();
	}

	void cycle() {
#ifndef ARM7TDMI_DISABLE_FIQ
		if(processFiq && !reg.fiqDisable) { [[unlikely]] // Service fast interrupt
				serviceFiq();
		} else
#endif
		if (processIrq && !reg.irqDisable) { [[unlikely]] // Service interrupt
				serviceIrq();
		} else {
			if (reg.thumbMode) {
				u16 lutIndex = pipelineOpcode3 >> 6;
				(this->*thumbLUT[lutIndex])((u16)pipelineOpcode3);
			} else {
				if (checkCondition(pipelineOpcode3 >> 28)) {
					u32 lutIndex = ((pipelineOpcode3 & 0x0FF00000) >> 16) | ((pipelineOpcode3 & 0x000000F0) >> 4);
					(this->*armLUT[lutIndex])(pipelineOpcode3);
				} else {
					fetchOpcode();
				}
			}
		}

#ifndef ARM7TDMI_DISABLE_DEBUG
		u32 nextInstrAddress = reg.R[15] - (reg.thumbMode ? 4 : 8);
		// Again, this is too smart for me
		if (const auto& breakpointBitmap = breakpointsTable[nextInstrAddress >> BMP_BITS];
			breakpointBitmap && breakpointBitmap->test(nextInstrAddress & BMP_MASK)) { [[unlikely]]
			bus.breakpoint();
		}
#endif
	}

	void addBreakpoint(u32 address) {
		auto& page = breakpointsTable[address >> BMP_BITS];
		if (!page) {
			page = std::make_unique<std::bitset<BMP_SIZE>>();
		}
		page->set(address & BMP_MASK);
	}

	void removeBreakpoint(u32 address) {
		auto& page = breakpointsTable[address >> BMP_BITS];
		if (!page) {
			return;
		}

		page->reset(address & BMP_MASK);
		if (page->none()) {
			page.reset();
		}
	}

	enum cpuMode {
		MODE_USER = 0x10,
		MODE_FIQ = 0x11,
		MODE_IRQ = 0x12,
		MODE_SUPERVISOR = 0x13,
		MODE_ABORT = 0x17,
		MODE_UNDEFINED = 0x1B,
		MODE_SYSTEM = 0x1F
	};
	struct {
		// Normal registers
		u32 R[16];

		// Status register
		union {
			struct {
				u32 mode : 5;
				u32 thumbMode : 1;
				u32 fiqDisable : 1;
				u32 irqDisable : 1;
				u32 : 20;
				u32 flagV : 1;
				u32 flagC : 1;
				u32 flagZ : 1;
				u32 flagN : 1;
			};
			u32 CPSR;
		};

		// Banked registers for each mode
		// Indexes 0-6 are R8-R14, 7 is SPSR
		u32 R_usr[8];
		u32 R_fiq[8];
		u32 R_svc[8];
		u32 R_abt[8];
		u32 R_irq[8];
		u32 R_und[8];
	} reg;

	/* Instruction Fetch/Decode */
	u32 pipelineOpcode1; // R15
	u32 pipelineOpcode2; // R15 + 4
	u32 pipelineOpcode3; // R15 + 8
	bool nextFetchType;

	bool checkCondition(int conditionCode) {
		switch (conditionCode) {
		case 0x0: return reg.flagZ;
		case 0x1: return !reg.flagZ;
		case 0x2: return reg.flagC;
		case 0x3: return !reg.flagC;
		case 0x4: return reg.flagN;
		case 0x5: return !reg.flagN;
		case 0x6: return reg.flagV;
		case 0x7: return !reg.flagV;
		case 0x8: return reg.flagC && !reg.flagZ;
		case 0x9: return !reg.flagC || reg.flagZ;
		case 0xA: return reg.flagN == reg.flagV;
		case 0xB: return reg.flagN != reg.flagV;
		case 0xC: return !reg.flagZ && (reg.flagN == reg.flagV);
		case 0xD: return reg.flagZ || (reg.flagN != reg.flagV);
		case 0xE: [[likely]] return true;
		case 0xF: [[unlikely]] return true;
		default:
			unknownOpcodeArm(pipelineOpcode3, "Invalid condition");
			return false;
		}
	}

	void serviceFiq() {
		//fetchOpcode();
		//reg.R[15] -= reg.thumbMode ? 2 : 4;
		bool oldThumb = reg.thumbMode;
		processFiq = false;
		bankRegisters(MODE_FIQ, true);
		reg.R[14] = reg.R[15] - (oldThumb ? 0 : 4);

		reg.irqDisable = true;
		reg.fiqDisable = true;
		reg.thumbMode = false;

		reg.R[15] = 0x0000001C;
		flushPipeline();
	}

	void serviceIrq() {
		//fetchOpcode();
		//reg.R[15] -= reg.thumbMode ? 2 : 4;
		bool oldThumb = reg.thumbMode;
		processIrq = false;
		bankRegisters(MODE_IRQ, true);
		reg.R[14] = reg.R[15] - (oldThumb ? 0 : 4);

		reg.irqDisable = true;
		reg.fiqDisable = true;
		reg.thumbMode = false;

		reg.R[15] = 0x00000018;
		flushPipeline();
	}

	void fetchOpcode() {
		if (reg.thumbMode) {
			pipelineOpcode1 = bus.template read<u16, true>(reg.R[15], nextFetchType);
			pipelineOpcode3 = pipelineOpcode2;
			pipelineOpcode2 = pipelineOpcode1;

			reg.R[15] += 2;
		} else {
			pipelineOpcode1 = bus.template read<u32, true>(reg.R[15], nextFetchType);
			pipelineOpcode3 = pipelineOpcode2;
			pipelineOpcode2 = pipelineOpcode1;

			reg.R[15] += 4;
		}

		nextFetchType = true;
	}

	void flushPipeline() {
		if (reg.thumbMode) {
			reg.R[15] = (reg.R[15] & ~1) + 4;
			pipelineOpcode3 = bus.template read<u16, true>(reg.R[15] - 4, false);
			pipelineOpcode2 = bus.template read<u16, true>(reg.R[15] - 2, true);
		} else {
			reg.R[15] = (reg.R[15] & ~3) + 8;
			pipelineOpcode3 = bus.template read<u32, true>(reg.R[15] - 8, false);
			pipelineOpcode2 = bus.template read<u32, true>(reg.R[15] - 4, true);
		}

		nextFetchType = true;
	}

	/* Errors */
	void unknownOpcodeArm(u32 opcode) {
		unknownOpcodeArm(opcode, "No LUT entry");
	}

	void unknownOpcodeArm(u32 opcode, std::string message) {
		bus.log << fmt::format("Unknown ARM opcode 0x{:0>8X} at address 0x{:0>7X}  Message: {}\n", opcode, reg.R[15] - 8, message.c_str());
		bus.hacf();
	}

	void unknownOpcodeThumb(u16 opcode) {
		unknownOpcodeThumb(opcode, "No LUT entry");
	}

	void unknownOpcodeThumb(u16 opcode, std::string message) {
		bus.log << fmt::format("Unknown THUMB opcode 0x{:0>4X} at address 0x{:0>7X}  Message: {}\n", opcode, reg.R[15] - 4, message.c_str());
		bus.hacf();
	}

	/* Helper Functions */
	template <typename TT> u32 rotateMisaligned(TT value, u32 address) {
		return std::rotr((u32)value, (address & (sizeof(TT) - 1)) * 8);
	}

	template <bool dataTransfer, bool iBit> bool computeShift(u32 opcode, u32 *result) {
		u32 shiftOperand;
		u32 shiftAmount;
		bool shifterCarry = false;

		if constexpr (dataTransfer && !iBit) {
			shiftOperand = opcode & 0xFFF;
		} else if constexpr (iBit && !dataTransfer) {
			shiftOperand = opcode & 0xFF;
			shiftAmount = (opcode & (0xF << 8)) >> 7;
			if (shiftAmount == 0) {
				shifterCarry = reg.flagC;
			} else {
				shifterCarry = shiftOperand & (1 << (shiftAmount - 1));
				shiftOperand = (shiftOperand >> shiftAmount) | (shiftOperand << (32 - shiftAmount));
			}
		} else {
			if (opcode & (1 << 4)) {
				shiftAmount = reg.R[(opcode >> 8) & 0xF] & 0xFF;
			} else {
				shiftAmount = (opcode >> 7) & 0x1F;
			}
			shiftOperand = reg.R[opcode & 0xF];

			if ((opcode & (1 << 4)) && (shiftAmount == 0)) {
				shifterCarry = reg.flagC;
			} else {
				switch ((opcode >> 5) & 3) {
				case 0: // LSL
					if (shiftAmount != 0) {
						if (shiftAmount > 31) {
							shifterCarry = (shiftAmount == 32) ? (shiftOperand & 1) : 0;
							shiftOperand = 0;
							break;
						}
						shifterCarry = shiftOperand & (1 << (31 - (shiftAmount - 1)));
						shiftOperand <<= shiftAmount;
					} else {
						shifterCarry = reg.flagC;
					}
					break;
				case 1: // LSR
					if ((shiftAmount == 0) || (shiftAmount == 32)) {
						shifterCarry = shiftOperand >> 31;
						shiftOperand = 0;
					} else if (shiftAmount > 32) {
						shiftOperand = 0;
						shifterCarry = false;
					} else {
						shifterCarry = (shiftOperand >> (shiftAmount - 1)) & 1;
						shiftOperand = shiftOperand >> shiftAmount;
					}
					break;
				case 2: // ASR
					if ((shiftAmount == 0) || (shiftAmount > 31)) {
						if (shiftOperand & (1 << 31)) {
							shiftOperand = 0xFFFFFFFF;
							shifterCarry = true;
						} else {
							shiftOperand = 0;
							shifterCarry = false;
						}
					} else {
						shifterCarry = (shiftOperand >> (shiftAmount - 1)) & 1;
						shiftOperand = ((i32)shiftOperand) >> shiftAmount;
					}
					break;
				case 3: // ROR
					if (opcode & (1 << 4)) { // Using register as shift amount
						if (shiftAmount == 0) {
							shifterCarry = reg.flagC;
							break;
						}
						shiftAmount &= 31;
						if (shiftAmount == 0) {
							shifterCarry = shiftOperand >> 31;
							break;
						}
					} else {
						if (shiftAmount == 0) { // RRX
							shifterCarry = shiftOperand & 1;
							shiftOperand = (shiftOperand >> 1) | (reg.flagC << 31);
							break;
						}
					}
					shifterCarry = shiftOperand & (1 << (shiftAmount - 1));
					shiftOperand = (shiftOperand >> shiftAmount) | (shiftOperand << (32 - shiftAmount));
					break;
				}
			}
		}

		*result = shiftOperand;
		return shifterCarry;
	}

	void bankRegisters(cpuMode newMode, bool enterMode) {
		u32 *currentModeBank = nullptr;
		switch (reg.mode) {
		case MODE_SYSTEM:
		case MODE_USER: currentModeBank = reg.R_usr; break;
		case MODE_FIQ: currentModeBank = reg.R_fiq; break;
		case MODE_IRQ: currentModeBank = reg.R_irq; break;
		case MODE_SUPERVISOR: currentModeBank = reg.R_svc; break;
		case MODE_ABORT: currentModeBank = reg.R_abt; break;
		case MODE_UNDEFINED: currentModeBank = reg.R_und; break;
		}

		u32 *newModeBank = nullptr;
		switch (newMode) {
		case MODE_SYSTEM:
		case MODE_USER: newModeBank = reg.R_usr; break;
		case MODE_FIQ: newModeBank = reg.R_fiq; break;
		case MODE_IRQ: newModeBank = reg.R_irq; break;
		case MODE_SUPERVISOR: newModeBank = reg.R_svc; break;
		case MODE_ABORT: newModeBank = reg.R_abt; break;
		case MODE_UNDEFINED: newModeBank = reg.R_und; break;
		default:
			printf("Invalid mode 0x%02X\n", newMode);
			bus.log << fmt::format("Invalid mode 0x{:0>2X}\n", (int)newMode);
			bus.hacf();
			return;
		}

		// Save current bank
		if (reg.mode == MODE_FIQ) {
			reg.R_fiq[0] = reg.R[8];
			reg.R_fiq[1] = reg.R[9];
			reg.R_fiq[2] = reg.R[10];
			reg.R_fiq[3] = reg.R[11];
			reg.R_fiq[4] = reg.R[12];
		} else { // User mode R8-R12 stores whatever was in R8-R12 so any mode can be switched to from fiq
			reg.R_usr[0] = reg.R[8];
			reg.R_usr[1] = reg.R[9];
			reg.R_usr[2] = reg.R[10];
			reg.R_usr[3] = reg.R[11];
			reg.R_usr[4] = reg.R[12];
		}
		currentModeBank[5] = reg.R[13];
		currentModeBank[6] = reg.R[14];

		// Load new bank
		if (newMode == MODE_FIQ) {
			reg.R[8] = reg.R_fiq[0];
			reg.R[9] = reg.R_fiq[1];
			reg.R[10] = reg.R_fiq[2];
			reg.R[11] = reg.R_fiq[3];
			reg.R[12] = reg.R_fiq[4];
		} else {
			reg.R[8] = reg.R_usr[0];
			reg.R[9] = reg.R_usr[1];
			reg.R[10] = reg.R_usr[2];
			reg.R[11] = reg.R_usr[3];
			reg.R[12] = reg.R_usr[4];
		}
		reg.R[13] = newModeBank[5];
		reg.R[14] = newModeBank[6];

		// Save SPSR and set new CPSR
		if (enterMode) {
			if (newMode != MODE_SYSTEM && newMode != MODE_USER)
				newModeBank[7] = reg.CPSR;
			reg.CPSR = (reg.CPSR & ~0x3F) | newMode;
		}
	}

	void leaveMode() {
		u32 tmpPSR = reg.CPSR;
		switch (reg.mode) {
		case MODE_FIQ: tmpPSR = reg.R_fiq[7]; break;
		case MODE_IRQ: tmpPSR = reg.R_irq[7]; break;
		case MODE_SUPERVISOR: tmpPSR = reg.R_svc[7]; break;
		case MODE_ABORT: tmpPSR = reg.R_abt[7]; break;
		case MODE_UNDEFINED: tmpPSR = reg.R_und[7]; break;
		}
		bankRegisters((cpuMode)(tmpPSR & 0x1F), false);
		reg.CPSR = tmpPSR;
	}

	/* ARM instructions */
	template <bool iBit, int operation, bool sBit> void dataProcessing(u32 opcode)  {
		// Shift and rotate to get operands
		u32 operand1;
		u32 operand2;

		bool shiftReg = !iBit && ((opcode >> 4) & 1);
		if (shiftReg) {
			fetchOpcode();
		}
		bool shifterCarry = computeShift<false, iBit>(opcode, &operand2);

		// Perform operation
		bool operationCarry = reg.flagC;
		bool operationOverflow = reg.flagV;
		operand1 = reg.R[(opcode >> 16) & 0xF];
		u32 result = 0;
		auto destinationReg = (opcode & (0xF << 12)) >> 12;
		switch (operation) {
		case 0x0: // AND
			result = operand1 & operand2;
			break;
		case 0x1: // EOR
			result = operand1 ^ operand2;
			break;
		case 0x2: // SUB
			operationCarry = operand1 >= operand2;
			result = operand1 - operand2;
			operationOverflow = ((operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 0x3: // RSB
			operationCarry = operand2 >= operand1;
			result = operand2 - operand1;
			operationOverflow = ((operand2 ^ operand1) & ((operand2 ^ result)) & 0x80000000) > 0;
			break;
		case 0x4: // ADD
			operationCarry = ((u64)operand1 + (u64)operand2) >> 32;
			result = operand1 + operand2;
			operationOverflow = (~(operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 0x5: // ADC
			operationCarry = ((u64)operand1 + (u64)operand2 + reg.flagC) >> 32;
			result = operand1 + operand2 + reg.flagC;
			operationOverflow = (~(operand1 ^ operand2) & ((operand1 ^ result))) >> 31;
			break;
		case 0x6: // SBC
			operationCarry = (u64)operand1 >= ((u64)operand2 + !reg.flagC);
			result = (u64)operand1 - ((u64)operand2 + !reg.flagC);
			operationOverflow = ((operand1 ^ operand2) & (operand1 ^ result)) >> 31;
			break;
		case 0x7: // RSC
			operationCarry = (u64)operand2 >= ((u64)operand1 + !reg.flagC);
			result = (u64)operand2 - ((u64)operand1 + !reg.flagC);
			operationOverflow = ((operand2 ^ operand1) & (operand2 ^ result)) >> 31;
			break;
		case 0x8: // TST
			result = operand1 & operand2;
			break;
		case 0x9: // TEQ
			result = operand1 ^ operand2;
			break;
		case 0xA: // CMP
			operationCarry = operand1 >= operand2;
			result = operand1 - operand2;
			operationOverflow = ((operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 0xB: // CMN
			operationCarry = ((u64)operand1 + (u64)operand2) >> 32;
			result = operand1 + operand2;
			operationOverflow = (~(operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 0xC: // ORR
			result = operand1 | operand2;
			break;
		case 0xD: // MOV
			result = operand2;
			break;
		case 0xE: // BIC
			result = operand1 & (~operand2);
			break;
		case 0xF: // MVN
			result = ~operand2;
			break;
		}

		// Compute common flags
		if constexpr (sBit) {
			reg.flagN = result >> 31;
			reg.flagZ = result == 0;
			if ((operation < 2) || (operation == 8) || (operation == 9) || (operation >= 0xC)) { // Logical operations
				reg.flagC = shifterCarry;
			} else {
				reg.flagC = operationCarry;
				reg.flagV = operationOverflow;
			}
		}

		if (shiftReg) {
			bus.iCycle(1); // TODO: Should probably be after setting register
		} else {
			fetchOpcode();
		}

		if constexpr ((operation < 8) || (operation >= 0xC)) {
			reg.R[destinationReg] = result;

			if (destinationReg == 15) {
				if (sBit)
					leaveMode();

				flushPipeline();
			}
		} else if constexpr (sBit) {
			if (destinationReg == 15)
				leaveMode();
		}
	}

	template <bool accumulate, bool sBit> void multiply(u32 opcode) {
		u32 destinationReg = (opcode >> 16) & 0xF;
		u32 multiplier = reg.R[(opcode >> 8) & 0xF];
		fetchOpcode();

		u32 result = multiplier * reg.R[opcode & 0xF];
		if constexpr (accumulate) {
			result += reg.R[(opcode >> 12) & 0xF];
			bus.iCycle(1);
		}
		if (destinationReg != 15)
			reg.R[destinationReg] = result;
		if constexpr (sBit) {
			reg.flagN = result >> 31;
			reg.flagZ = result == 0;
		}

		int multiplierCycles = ((31 - std::max(std::countl_zero(multiplier), std::countl_one(multiplier))) / 8) + 1;
		bus.iCycle(multiplierCycles);
	}

	template <bool signedMul, bool accumulate, bool sBit> void multiplyLong(u32 opcode) {
		u32 destinationRegLow = (opcode >> 12) & 0xF;
		u32 destinationRegHigh = (opcode >> 16) & 0xF;
		u32 multiplier = reg.R[(opcode >> 8) & 0xF];
		fetchOpcode();

		u64 result;
		int multiplierCycles;
		if constexpr (signedMul) {
			result = (i64)((i32)multiplier) * (i64)((i32)reg.R[opcode & 0xF]);
			multiplierCycles = ((31 - std::max(std::countl_zero(multiplier), std::countl_one(multiplier))) / 8) + 1;
		} else {
			result = (u64)multiplier * (u64)reg.R[opcode & 0xF];
			multiplierCycles = ((31 - std::countl_zero(multiplier)) / 8) + 1;
		}
		if constexpr (accumulate) {
			result += ((u64)reg.R[destinationRegHigh] << 32) | (u64)reg.R[destinationRegLow];
			bus.iCycle(1);
		}
		if constexpr (sBit) {
			reg.flagN = result >> 63;
			reg.flagZ = result == 0;
		}

		bus.iCycle(multiplierCycles + 1);

		if (destinationRegLow != 15)
			reg.R[destinationRegLow] = result;
		if (destinationRegHigh != 15)
			reg.R[destinationRegHigh] = result >> 32;
	}

	template <bool byteWord> void singleDataSwap(u32 opcode) {
		u32 address = reg.R[(opcode >> 16) & 0xF];
		u32 sourceRegister = opcode & 0xF;
		u32 destinationRegister = (opcode >> 12) & 0xF;
		u32 result;
		fetchOpcode();

		if constexpr (byteWord) {
			result = bus.template read<u8, false>(address, true);
			bus.template write<u8>(address, (u8)reg.R[sourceRegister], false);
		} else {
			result = rotateMisaligned(bus.template read<u32, false>(address, true), address);
			bus.template write<u32>(address, reg.R[sourceRegister], false);
		}

		reg.R[destinationRegister] = result;
		bus.iCycle(1);

		if (destinationRegister == 15) {
			flushPipeline();
		}
	}

	template <bool targetPSR> void psrLoad(u32 opcode) {
		u32 destinationReg = (opcode >> 12) & 0xF;

		if constexpr (targetPSR) {
			switch (reg.mode) {
			case MODE_FIQ: reg.R[destinationReg] = reg.R_fiq[7]; break;
			case MODE_IRQ: reg.R[destinationReg] = reg.R_irq[7]; break;
			case MODE_SUPERVISOR: reg.R[destinationReg] = reg.R_svc[7]; break;
			case MODE_ABORT: reg.R[destinationReg] = reg.R_abt[7]; break;
			case MODE_UNDEFINED: reg.R[destinationReg] = reg.R_und[7]; break;
			default: reg.R[destinationReg] = reg.CPSR; break;
			}
		} else {
			reg.R[destinationReg] = reg.CPSR;
		}

		fetchOpcode();
	}

	template <bool targetPSR> void psrStoreReg(u32 opcode) {
		u32 operand = reg.R[opcode & 0xF];

		u32 *target;
		if constexpr (targetPSR) {
			switch (reg.mode) {
			case MODE_FIQ: target = &reg.R_fiq[7]; break;
			case MODE_IRQ: target = &reg.R_irq[7]; break;
			case MODE_SUPERVISOR: target = &reg.R_svc[7]; break;
			case MODE_ABORT: target = &reg.R_abt[7]; break;
			case MODE_UNDEFINED: target = &reg.R_und[7]; break;
			default:
				fetchOpcode();
				return;
			}
		} else {
			target = &reg.CPSR;
		}

		u32 result = 0;
		if (opcode & (1 << 19)) {
			result |= operand & 0xF0000000;
		} else {
			result |= *target & 0xF0000000;
		}
		if ((opcode & (1 << 16)) && reg.mode != MODE_USER) {
			result |= operand & 0x000000FF;
			if constexpr (!targetPSR)
				bankRegisters((cpuMode)(operand & 0x1F), false);
		} else {
			result |= *target & 0x000000FF;
		}

#ifdef ARM7TDMI_DISABLE_FIQ
		result |= 0x00000040;
#endif
		*target = result | 0x00000010; // M[4] is always 1
		fetchOpcode();
	}

	template <bool targetPSR> void psrStoreImmediate(u32 opcode) {
		u32 operand = opcode & 0xFF;
		u32 shiftAmount = (opcode & (0xF << 8)) >> 7;
		operand = shiftAmount ? ((operand >> shiftAmount) | (operand << (32 - shiftAmount))) : operand;

		u32 *target;
		if constexpr (targetPSR) {
			switch (reg.mode) {
			case MODE_FIQ: target = &reg.R_fiq[7]; break;
			case MODE_IRQ: target = &reg.R_irq[7]; break;
			case MODE_SUPERVISOR: target = &reg.R_svc[7]; break;
			case MODE_ABORT: target = &reg.R_abt[7]; break;
			case MODE_UNDEFINED: target = &reg.R_und[7]; break;
			default: fetchOpcode(); return;
			}
		} else {
			target = &reg.CPSR;
		}

		u32 result = 0;
		if (opcode & (1 << 19)) {
			result |= operand & 0xF0000000;
		} else {
			result |= *target & 0xF0000000;
		}
		if ((opcode & (1 << 16)) && reg.mode != MODE_USER) {
			result |= operand & 0x000000FF;
			if constexpr (!targetPSR)
				bankRegisters((cpuMode)(operand & 0x1F), false);
		} else {
			result |= *target & 0x000000FF;
		}

#ifdef ARM7TDMI_DISABLE_FIQ
		result |= 0x00000040;
#endif
		*target = result | 0x00000010; // M[4] is always 1
		fetchOpcode();
	}

	void branchExchange(u32 opcode) {
		bool newThumb = reg.R[opcode & 0xF] & 1;
		u32 newAddress = reg.R[opcode & 0xF] & (newThumb ? ~1 : ~3);
		fetchOpcode();

		reg.thumbMode = newThumb;
		reg.R[15] = newAddress;
		flushPipeline();
	}

	template <bool prePostIndex, bool upDown, bool immediateOffset, bool writeBack, bool loadStore, int shBits> void halfwordDataTransfer(u32 opcode)  {
		auto baseRegister = (opcode >> 16) & 0xF;
		auto srcDestRegister = (opcode >> 12) & 0xF;
		if ((baseRegister == 15) && writeBack)
			unknownOpcodeArm(opcode, "r15 Operand With Writeback");

		u32 offset;
		if constexpr (immediateOffset) {
			offset = ((opcode & 0xF00) >> 4) | (opcode & 0xF);
		} else {
			offset = reg.R[opcode & 0xF];
		}

		u32 address = reg.R[baseRegister];
		if constexpr (prePostIndex) {
			if constexpr (upDown) {
				address += offset;
			} else {
				address -= offset;
			}
		}
		fetchOpcode();

		u32 result = 0;
		if constexpr (loadStore) {
			if constexpr (shBits == 1) { // LDRH
				result = rotateMisaligned(bus.template read<u16, false>(address, false), address);
			} else if constexpr (shBits == 2) { // LDRSB
				result = ((i32)((u32)bus.template read<u8, false>(address, false) << 24) >> 24);
			} else if constexpr (shBits == 3) { // LDRSH
				result = rotateMisaligned(bus.template read<u16, false>(address, false), address);

				if (address & 1) {
					result = (i32)(result << 24) >> 24;
				} else {
					result = (i32)(result << 16) >> 16;
				}
			}
		} else {
			if constexpr (shBits == 1) { // STRH
				bus.template write<u16>(address, (u16)reg.R[srcDestRegister], false);
			}

			nextFetchType = false;
		}

		// TODO: Base register modification should be at same time as read/write
		if constexpr (writeBack && prePostIndex)
			reg.R[baseRegister] = address;
		if constexpr (!prePostIndex) {
			if constexpr (upDown) {
				address += offset;
			} else {
				address -= offset;
			}
			reg.R[baseRegister] = address;
		}
		if constexpr (loadStore) {
			reg.R[srcDestRegister] = result;
			bus.iCycle(1);

			if (srcDestRegister == 15) {
				flushPipeline();
			}
		}
	}

	template <bool immediateOffset, bool prePostIndex, bool upDown, bool byteWord, bool writeBack, bool loadStore> void singleDataTransfer(u32 opcode) {
		auto baseRegister = (opcode >> 16) & 0xF;
		auto srcDestRegister = (opcode >> 12) & 0xF;
		if constexpr (writeBack)
			if (baseRegister == 15)
				unknownOpcodeArm(opcode, "r15 Operand With Writeback");

		u32 offset;
		computeShift<true, immediateOffset>(opcode, &offset);

		u32 address = reg.R[baseRegister];
		if constexpr (prePostIndex) {
			if constexpr (upDown) {
				address += offset;
			} else {
				address -= offset;
			}
		}
		fetchOpcode();

		u32 result = 0;
		if constexpr (loadStore) { // LDR
			if constexpr (byteWord) {
				result = bus.template read<u8, false>(address, false);
			} else {
				result = rotateMisaligned(bus.template read<u32, false>(address, false), address);
			}
		} else { // STR
			if constexpr (byteWord) {
				bus.template write<u8>(address, reg.R[srcDestRegister], false);
			} else {
				bus.template write<u32>(address, reg.R[srcDestRegister], false);
			}

			nextFetchType = false;
		}

		// TODO: Base register modification should be at same time as read/write
		if constexpr (writeBack && prePostIndex)
			reg.R[baseRegister] = address;
		if constexpr (!prePostIndex) {
			if constexpr (upDown) {
				address += offset;
			} else {
				address -= offset;
			}
			reg.R[baseRegister] = address;
		}
		if constexpr (loadStore) {
			reg.R[srcDestRegister] = result;
			bus.iCycle(1);

			if (srcDestRegister == 15) {
				flushPipeline();
			}
		}
	}

	void undefined(u32 opcode) {
		bankRegisters(MODE_UNDEFINED, true);
		reg.R[14] = reg.R[15] - 4;
		fetchOpcode();

		reg.R[15] = 0x4;
		flushPipeline();
	}

	template <bool prePostIndex, bool upDown, bool sBit, bool writeBack, bool loadStore> void blockDataTransfer(u32 opcode) {
		const u32 baseRegister = (opcode >> 16) & 0xF;
		const bool useAltRegisterBank = sBit && !(loadStore && (opcode & (1 << 15))) && reg.mode != MODE_USER && reg.mode != MODE_SYSTEM;
		if ((baseRegister == 15) && writeBack)
			unknownOpcodeArm(opcode, "LDM/STM has r15 as the Base Register When Writeback is Enabled");

		u32 address = reg.R[baseRegister];
		u32 writeBackAddress;
		bool emptyRegList = (opcode & 0xFFFF) == 0;
		if constexpr (upDown) { // I
			writeBackAddress = address + std::popcount(opcode & 0xFFFF) * 4;
			if (emptyRegList)
				writeBackAddress += 0x40;

			if constexpr (prePostIndex)
				address += 4;
		} else { // D
			address -= std::popcount(opcode & 0xFFFF) * 4;
			if (emptyRegList)
				address -= 0x40;
			writeBackAddress = address;

			if constexpr (!prePostIndex)
				address += 4;
		}

		fetchOpcode();

		bool firstReadWrite = true; // TODO: Interleave fetches with register writes
		if constexpr (loadStore) { // LDM
			if (emptyRegList) { // TODO: find timings for empty list
				if constexpr (writeBack)
					reg.R[baseRegister] = writeBackAddress;
				reg.R[15] = bus.template read<u32, false>(address, false);
				flushPipeline();
			} else {
				for (int i = 0; i < 16; i++) {
					if (opcode & (1 << i)) {
						if (firstReadWrite) {
							if constexpr (writeBack)
								reg.R[baseRegister] = writeBackAddress;
						}

						u32 value = bus.template read<u32, false>(address, !firstReadWrite);
						if (useAltRegisterBank && i >= (reg.mode == MODE_FIQ ? 8 : 13) && i != 15) {
							reg.R_usr[i - 8] = value;
						} else {
							reg.R[i] = value;
						}
						address += 4;

						if (firstReadWrite)
							firstReadWrite = false;
					}
				}
				bus.iCycle(1);

				if (opcode & (1 << 15)) { // Treat r15 loads as jumps
					flushPipeline();
				}
			}
		} else { // STM
			if (emptyRegList) {
				bus.template write<u32>(address, reg.R[15], false);
				if constexpr (writeBack)
					reg.R[baseRegister] = writeBackAddress;
			} else {
				for (int i = 0; i < 16; i++) {
					if (opcode & (1 << i)) {
						if (useAltRegisterBank && i >= (reg.mode == MODE_FIQ ? 8 : 13) && i != 15) {
							bus.template write<u32>(address, reg.R_usr[i - 8], !firstReadWrite);
						} else {
							bus.template write<u32>(address, reg.R[i], !firstReadWrite);
						}
						address += 4;

						if (firstReadWrite) {
							if constexpr (writeBack)
								reg.R[baseRegister] = writeBackAddress;
							firstReadWrite = false;
						}
					}
				}
			}

			nextFetchType = false;
		}

		if (sBit && loadStore && (opcode & (1 << 15))) {
			leaveMode();
		}
	}

	template <bool link> void branch(u32 opcode) {
		u32 address = reg.R[15] + (((i32)((opcode & 0x00FFFFFF) << 8)) >> 6);
		fetchOpcode();

		if constexpr (link)
			reg.R[14] = reg.R[15] - 8;
		reg.R[15] = address;
		flushPipeline();
	}

	// This is just barely stubbed to pass a test
	template <bool loadStore> void armCoprocessorRegisterTransfer(u32 opcode) {
		u32 copOpc = (opcode >> 21) & 0x7;
		u32 copSrcDestReg = (opcode >> 16) & 0xF;
		u32 srcDestRegister = (opcode >> 12) & 0xF;
		u32 copNum = (opcode >> 8) & 0xF;
		u32 copOpcType = (opcode >> 5) & 0x7;
		u32 copOpReg = opcode & 0xF;

		if (copNum == 14) {
			fetchOpcode();
		} else {
			undefined(opcode);
			return;
		}
	}

	void softwareInterrupt(u32 opcode) { // TODO: Proper timings for exceptions
		fetchOpcode();
		bankRegisters(MODE_SUPERVISOR, true);
		reg.R[14] = reg.R[15] - 8;

		reg.R[15] = 0x8;
		flushPipeline();
	}

	/* THUMB Instructions */
	template <int op, int shiftAmount> void thumbMoveShiftedReg(u16 opcode)  {
		u32 shiftOperand = reg.R[(opcode >> 3) & 7];

		switch (op) {
		case 0: // LSL
			if (shiftAmount != 0) {
				if (shiftAmount > 31) {
					reg.flagC = (shiftAmount == 32) ? (shiftOperand & 1) : 0;
					shiftOperand = 0;
					break;
				}
				reg.flagC = (bool)(shiftOperand & (1 << (31 - (shiftAmount - 1))));
				shiftOperand <<= shiftAmount;
			}
			break;
		case 1: // LSR
			if (shiftAmount == 0) {
				reg.flagC = shiftOperand >> 31;
				shiftOperand = 0;
			} else {
				reg.flagC = (shiftOperand >> (shiftAmount - 1)) & 1;
				shiftOperand = shiftOperand >> shiftAmount;
			}
			break;
		case 2: // ASR
			if (shiftAmount == 0) {
				if (shiftOperand & (1 << 31)) {
					shiftOperand = 0xFFFFFFFF;
					reg.flagC = true;
				} else {
					shiftOperand = 0;
					reg.flagC = false;
				}
			} else {
				reg.flagC = (shiftOperand >> (shiftAmount - 1)) & 1;
				shiftOperand = ((i32)shiftOperand) >> shiftAmount;
			}
			break;
		}

		reg.flagN = shiftOperand >> 31;
		reg.flagZ = shiftOperand == 0;
		reg.R[opcode & 7] = shiftOperand;
		fetchOpcode();
	}

	template <bool immediate, bool op, int offset> void thumbAddSubtract(u16 opcode) {
		u32 operand1 = reg.R[(opcode >> 3) & 7];
		u32 operand2 = immediate ? offset : reg.R[offset];

		u32 result;
		if (op) { // SUB
			reg.flagC = operand1 >= operand2;
			result = operand1 - operand2;
			reg.flagV = ((operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			reg.flagN = result >> 31;
			reg.flagZ = result == 0;
		} else { // ADD
			reg.flagC = ((u64)operand1 + (u64)operand2) >> 32;
			result = operand1 + operand2;
			reg.flagV = (~(operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			reg.flagN = result >> 31;
			reg.flagZ = result == 0;
		}

		reg.R[opcode & 7] = result;
		fetchOpcode();
	}

	template <int op, int destinationReg> void thumbAluImmediate(u16 opcode) {
		u32 operand1 = reg.R[destinationReg];
		u32 operand2 = opcode & 0xFF;

		u32 result;
		switch (op) {
		case 0: // MOV
			result = operand2;
			break;
		case 1: // CMP
			reg.flagC = operand1 >= operand2;
			result = operand1 - operand2;
			reg.flagV = ((operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 2: // ADD
			reg.flagC = ((u64)operand1 + (u64)operand2) >> 32;
			result = operand1 + operand2;
			reg.flagV = (~(operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 3: // SUB
			reg.flagC = operand1 >= operand2;
			result = operand1 - operand2;
			reg.flagV = ((operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		}

		reg.flagN = result >> 31;
		reg.flagZ = result == 0;
		if constexpr (op != 1)
			reg.R[destinationReg] = result;
		fetchOpcode();
	}

	template <int op> void thumbAluReg(u16 opcode) {
		u32 destinationReg = opcode & 7;
		u32 operand1 = reg.R[destinationReg];
		u32 operand2 = reg.R[(opcode >> 3) & 7];

		constexpr bool writeResult = ((op != 0x8) && (op != 0xA) && (op != 0xB));
		constexpr bool endWithIdle = ((op == 0x2) || (op == 0x3) || (op == 0x4) || (op == 0x7) || (op == 0xD));

		u32 result;
		switch (op) {
		case 0x0: // AND
			result = operand1 & operand2;
			break;
		case 0x1: // EOR
			result = operand1 ^ operand2;
			break;
		case 0x2: // LSL
			if (operand2 == 0) {
				result = operand1;
			} else {
				if (operand2 > 31) {
					reg.flagC = (operand2 == 32) ? (operand1 & 1) : 0;
					result = 0;
				} else {
					reg.flagC = (operand1 & (1 << (31 - (operand2 - 1)))) > 0;
					result = operand1 << operand2;
				}
			}
			fetchOpcode();
			break;
		case 0x3: // LSR
			if (operand2 == 0) {
				result = operand1;
			} else if (operand2 == 32) {
				result = 0;
				reg.flagC = operand1 >> 31;
			} else if (operand2 > 32) {
				result = 0;
				reg.flagC = false;
			} else {
				reg.flagC = (operand1 >> (operand2 - 1)) & 1;
				result = operand1 >> operand2;
			}
			fetchOpcode();
			break;
		case 0x4: // ASR
			if (operand2 == 0) {
				result = operand1;
			} else if (operand2 > 31) {
				if (operand1 & (1 << 31)) {
					result = 0xFFFFFFFF;
					reg.flagC = true;
				} else {
					result = 0;
					reg.flagC = false;
				}
			} else {
				reg.flagC = (operand1 >> (operand2 - 1)) & 1;
				result = ((i32)operand1) >> operand2;
			}
			fetchOpcode();
			break;
		case 0x5: // ADC
			result = operand1 + operand2 + reg.flagC;
			reg.flagC = ((u64)operand1 + (u64)operand2 + reg.flagC) >> 32;
			reg.flagV = (~(operand1 ^ operand2) & ((operand1 ^ result))) >> 31;
			break;
		case 0x6: // SBC
			result = (u64)operand1 - ((u64)operand2 + !reg.flagC);
			reg.flagC = (u64)operand1 >= ((u64)operand2 + !reg.flagC);
			reg.flagV = ((operand1 ^ operand2) & (operand1 ^ result)) >> 31;
			break;
		case 0x7: // ROR
			if (operand2 == 0) {
				result = operand1;
			} else {
				operand2 &= 31;
				if (operand2 == 0) {
					reg.flagC = operand1 >> 31;
					result = operand1;
				} else {
					reg.flagC = (bool)(operand1 & (1 << (operand2 - 1)));
					result = (operand1 >> operand2) | (operand1 << (32 - operand2));
				}
			}
			fetchOpcode();
			break;
		case 0x8: // TST
			result = operand1 & operand2;
			break;
		case 0x9: // NEG
			reg.flagC = 0 >= operand2;
			result = 0 - operand2;
			reg.flagV = (operand2 & result & 0x80000000) > 0;
			break;
		case 0xA: // CMP
			reg.flagC = operand1 >= operand2;
			result = operand1 - operand2;
			reg.flagV = ((operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 0xB: // CMN
			reg.flagC = ((u64)operand1 + (u64)operand2) >> 32;
			result = operand1 + operand2;
			reg.flagV = (~(operand1 ^ operand2) & ((operand1 ^ result)) & 0x80000000) > 0;
			break;
		case 0xC: // ORR
			result = operand1 | operand2;
			break;
		case 0xD: // MUL
			fetchOpcode();
			bus.iCycle((31 - std::max(std::countl_zero(operand1), std::countl_one(operand1))) / 8);

			result = operand1 * operand2;
			break;
		case 0xE: // BIC
			result = operand1 & (~operand2);
			break;
		case 0xF: // MVN
			result = ~operand2;
			break;
		}

		// Compute common flags
		reg.flagN = result >> 31;
		reg.flagZ = result == 0;

		if constexpr (writeResult)
			reg.R[destinationReg] = result;
		if constexpr (endWithIdle) {
			bus.iCycle(1);
		} else {
			fetchOpcode();
		}
	}

	template <int op, bool opFlag1, bool opFlag2> void thumbHighRegOperation(u16 opcode) {
		u32 operand1 = (opcode & 0x7) + (opFlag1 ? 8 : 0);
		u32 operand2 = ((opcode >> 3) & 0x7) + (opFlag2 ? 8 : 0);

		u32 result;
		switch (op) {
		case 0: // ADD
			result = reg.R[operand1] + reg.R[operand2];
			break;
		case 1: // CMP
			reg.flagC = reg.R[operand1] >= reg.R[operand2];
			result = reg.R[operand1] - reg.R[operand2];
			reg.flagV = ((reg.R[operand1] ^ reg.R[operand2]) & ((reg.R[operand1] ^ result)) & 0x80000000) > 0;
			reg.flagN = result >> 31;
			reg.flagZ = result == 0;
			break;
		case 2: // MOV
			result = reg.R[operand2];
			break;
		case 3:{ // BX
			bool newThumb = reg.R[operand2] & 1;
			u32 newAddress = reg.R[operand2];
			fetchOpcode();

			reg.thumbMode = newThumb;
			reg.R[15] = newAddress;
			flushPipeline();
		}return;
		}
		fetchOpcode();
		if constexpr (op != 1)
			reg.R[operand1] = result;

		if (operand1 == 15) {
			flushPipeline();
		}
	}

	template <int destinationReg> void thumbPcRelativeLoad(u16 opcode) {
		u32 address = (reg.R[15] + ((opcode & 0xFF) << 2)) & ~3;
		fetchOpcode();

		reg.R[destinationReg] = rotateMisaligned(bus.template read<u32, false>(address, false), address);
		bus.iCycle(1);
	}

	template <bool loadStore, bool byteWord, int offsetReg> void thumbLoadStoreRegOffset(u16 opcode) {
		auto srcDestRegister = opcode & 0x7;
		u32 address = reg.R[(opcode >> 3) & 7] + reg.R[offsetReg];
		fetchOpcode();

		if constexpr (loadStore) {
			if constexpr (byteWord) { // LDRB
				reg.R[srcDestRegister] = bus.template read<u8, false>(address, false);
			} else { // LDR
				reg.R[srcDestRegister] = rotateMisaligned(bus.template read<u32, false>(address, false), address);
			}

			bus.iCycle(1);
		} else {
			if constexpr (byteWord) { // STRB
				bus.template write<u8>(address, (u8)reg.R[srcDestRegister], false);
			} else { // STR
				bus.template write<u32>(address, reg.R[srcDestRegister], false);
			}

			nextFetchType = false;
		}
	}

	template <int hsBits, int offsetReg> void thumbLoadStoreSext(u16 opcode) {
		auto srcDestRegister = opcode & 0x7;
		u32 address = reg.R[(opcode >> 3) & 7] + reg.R[offsetReg];
		fetchOpcode();

		u32 result = 0;
		switch (hsBits) {
		case 0: // STRH
			bus.template write<u16>(address, (u16)reg.R[srcDestRegister], false);
			nextFetchType = false;
			break;
		case 1: // LDSB
			result = bus.template read<u8, false>(address, false);
			result = (i32)(result << 24) >> 24;
			break;
		case 2: // LDRH
			result = rotateMisaligned(bus.template read<u16, false>(address, false), address);
			break;
		case 3: // LDSH
			result = rotateMisaligned(bus.template read<u16, false>(address, false), address);

			if (address & 1) {
				result = (i32)(result << 24) >> 24;
			} else {
				result = (i32)(result << 16) >> 16;
			}
			break;
		}

		if constexpr (hsBits != 0) {
			reg.R[srcDestRegister] = result;
			bus.iCycle(1);
		}
	}

	template <bool byteWord, bool loadStore, int offset> void thumbLoadStoreImmediateOffset(u16 opcode) {
		auto srcDestRegister = opcode & 0x7;
		u32 address = reg.R[(opcode >> 3) & 7] + (byteWord ? offset : (offset << 2));
		fetchOpcode();

		if constexpr (loadStore) {
			if constexpr (byteWord) { // LDRB
				reg.R[srcDestRegister] = bus.template read<u8, false>(address, false);
			} else { // LDR
				reg.R[srcDestRegister] = rotateMisaligned(bus.template read<u32, false>(address, false), address);
			}
			bus.iCycle(1);
		} else {
			if constexpr (byteWord) { // STRB
				bus.template write<u8>(address, (u8)reg.R[srcDestRegister], false);
			} else { // STR
				bus.template write<u32>(address, reg.R[srcDestRegister], false);
			}

			nextFetchType = false;
		}
	}

	template <bool loadStore, int offset> void thumbLoadStoreHalfword(u16 opcode) {
		auto srcDestRegister = opcode & 0x7;
		u32 address = reg.R[(opcode >> 3) & 7] + (offset << 1);
		fetchOpcode();

		if constexpr (loadStore) { // LDRH
			reg.R[srcDestRegister] = rotateMisaligned(bus.template read<u16, false>(address, false), address);

			bus.iCycle(1);
		} else { // STRH
			bus.template write<u16>(address, (u16)reg.R[srcDestRegister], false);

			nextFetchType = false;
		}
	}

	template <bool loadStore, int destinationReg> void thumbSpRelativeLoadStore(u16 opcode) {
		u32 address = reg.R[13] + ((opcode & 0xFF) << 2);
		fetchOpcode();

		if constexpr (loadStore) {
			reg.R[destinationReg] = bus.template read<u32, false>(address, false);

			bus.iCycle(1);
		} else {
			bus.template write<u32>(address, reg.R[destinationReg], false);

			nextFetchType = false;
		}
	}

	template <bool spPc, int destinationReg> void thumbLoadAddress(u16 opcode) {
		if constexpr (spPc) {
			reg.R[destinationReg] = reg.R[13] + ((opcode & 0xFF) << 2);
		} else {
			reg.R[destinationReg] = (reg.R[15] & ~3) + ((opcode & 0xFF) << 2);
		}
		fetchOpcode();
	}

	template <bool isNegative> void thumbSpAddOffset(u16 opcode) {
		u32 operand = (opcode & 0x7F) << 2;

		if constexpr (isNegative) {
			reg.R[13] -= operand;
		} else {
			reg.R[13] += operand;
		}
		fetchOpcode();
	}

	template <bool loadStore, bool pcLr> void thumbPushPopRegisters(u16 opcode) {
		u32 address = reg.R[13];
		bool emptyRegList = ((opcode & 0xFF) == 0) && !pcLr;

		bool firstReadWrite = true;
		if constexpr (loadStore) { // POP/LDMIA!
			u32 writeBackAddress = address + std::popcount((u32)opcode & 0xFF) * 4;
			if (emptyRegList)
				writeBackAddress += 0x40;
			reg.R[13] = writeBackAddress + (pcLr * 4);
			fetchOpcode(); // Writeback really should be inside the main loop but this works

			if (emptyRegList) {
				reg.R[15] = bus.template read<u32, false>(address, false);
				flushPipeline();
			} else {
				for (int i = 0; i < 8; i++) {
					if (opcode & (1 << i)) {
						reg.R[i] = bus.template read<u32, false>(address, !firstReadWrite);
						address += 4;

						if (firstReadWrite)
							firstReadWrite = false;
					}
				}
				bus.iCycle(1);
				if constexpr (pcLr) {
					reg.R[15] = bus.template read<u32, false>(address, true);
					flushPipeline();
				}
			}
		} else { // PUSH/STMDB!
			address -= (std::popcount((u32)opcode & 0xFF) + pcLr) * 4;
			if (emptyRegList)
				address -= 0x40;
			reg.R[13] = address;
			fetchOpcode();

			if (emptyRegList) {
				bus.template write<u32>(address, reg.R[15] + 2, false);
			} else {
				for (int i = 0; i < 8; i++) {
					if (opcode & (1 << i)) {
						bus.template write<u32>(address, reg.R[i], !firstReadWrite);
						address += 4;
					}
				}
				if constexpr (pcLr)
					bus.template write<u32>(address, reg.R[14], true);
			}
			nextFetchType = false;
		}
	}

	template <bool loadStore, int baseReg> void thumbMultipleLoadStore(u16 opcode) {
		u32 address = reg.R[baseReg];
		u32 writeBackAddress;
		bool emptyRegList = (opcode & 0xFF) == 0;

		writeBackAddress = address + std::popcount((u32)opcode & 0xFF) * 4;
		if (emptyRegList)
			writeBackAddress += 0x40;
		fetchOpcode();

		bool firstReadWrite = true;
		if constexpr (loadStore) { // LDMIA!
			if (emptyRegList) {
				reg.R[baseReg] = writeBackAddress;
				reg.R[15] = bus.template read<u32, false>(address, true);
				flushPipeline();
			} else {
				for (int i = 0; i < 8; i++) {
					if (opcode & (1 << i)) {
						if (firstReadWrite)
							reg.R[baseReg] = writeBackAddress;

						reg.R[i] = bus.template read<u32, false>(address, !firstReadWrite);
						address += 4;

						if (firstReadWrite)
							firstReadWrite = false;
					}
				}
				bus.iCycle(1);
			}
		} else { // STMIA!
			if (emptyRegList) {
				bus.template write<u32>(address, reg.R[15], false);
				reg.R[baseReg] = writeBackAddress;
			} else {
				for (int i = 0; i < 8; i++) {
					if (opcode & (1 << i)) {
						bus.template write<u32>(address, reg.R[i], !firstReadWrite);
						address += 4;

						if (firstReadWrite) {
							reg.R[baseReg] = writeBackAddress;
							firstReadWrite = false;
						}
					}
				}
			}
			nextFetchType = false;
		}
	}

	template <int condition> void thumbConditionalBranch(u16 opcode) {
		u32 newAddress = reg.R[15] + ((i16)(opcode << 8) >> 7);
		fetchOpcode();

		if (checkCondition(condition)) {
			reg.R[15] = newAddress;
			flushPipeline();
		}
	}

	void thumbUndefined(u16 opcode) {
		bankRegisters(MODE_UNDEFINED, true);
		reg.R[14] = reg.R[15] - 2;
		fetchOpcode();

		reg.R[15] = 0x4;
		flushPipeline();
	}

	void thumbSoftwareInterrupt(u16 opcode) {
		fetchOpcode();
		bankRegisters(MODE_SUPERVISOR, true);
		reg.R[14] = reg.R[15] - 4;

		reg.R[15] = 0x8;
		flushPipeline();
	}

	void thumbUnconditionalBranch(u16 opcode) {
		u32 newAddress = reg.R[15] + ((i16)(opcode << 5) >> 4);
		fetchOpcode();

		reg.R[15] = newAddress;
		flushPipeline();
	}

	template <bool lowHigh> void thumbLongBranchLink(u16 opcode) {
		if constexpr (lowHigh) {
			u32 address = reg.R[14] + ((opcode & 0x7FF) << 1);
			reg.R[14] = (reg.R[15] - 2) | 1;
			fetchOpcode();

			reg.R[15] = address;
			flushPipeline();
		} else {
			reg.R[14] = reg.R[15] + ((i32)((u32)opcode << 21) >> 9);
			fetchOpcode();
		}
	}

	/* Generate Instruction LUTs */
	static const u32 armUndefined1Mask = 0b1111'1011'0000;
	static const u32 armUndefined1Bits = 0b0011'0000'0000;
	static const u32 armUndefined2Mask = 0b1110'0000'0001;
	static const u32 armUndefined2Bits = 0b0110'0000'0001;
	static const u32 armUndefined3Mask = 0b1'1111'1111'1111;
	static const u32 armUndefined3Bits = 0b0'0001'0110'0001;
	static const u32 armUndefined4Mask = 0b1'1111'1001'1111;
	static const u32 armUndefined4Bits = 0b0'0001'0000'0101;
	static const u32 armDataProcessingMask = 0b1100'0000'0000;
	static const u32 armDataProcessingBits = 0b0000'0000'0000;
	static const u32 armMultiplyMask = 0b1111'1100'1111;
	static const u32 armMultiplyBits = 0b0000'0000'1001;
	static const u32 armMultiplyLongMask = 0b1111'1000'1111;
	static const u32 armMultiplyLongBits = 0b0000'1000'1001;
	static const u32 armPsrLoadMask = 0b1111'1011'1111;
	static const u32 armPsrLoadBits = 0b0001'0000'0000;
	static const u32 armPsrStoreRegMask = 0b1111'1011'1111;
	static const u32 armPsrStoreRegBits = 0b0001'0010'0000;
	static const u32 armPsrStoreImmediateMask = 0b1111'1011'0000;
	static const u32 armPsrStoreImmediateBits = 0b0011'0010'0000;
	static const u32 armSingleDataSwapMask = 0b1111'1011'1111;
	static const u32 armSingleDataSwapBits = 0b0001'0000'1001;
	static const u32 armBranchExchangeMask = 0b1111'1111'1111;
	static const u32 armBranchExchangeBits = 0b0001'0010'0001;
	static const u32 armHalfwordDataTransferMask = 0b1110'0000'1001;
	static const u32 armHalfwordDataTransferBits = 0b0000'0000'1001;
	static const u32 armSingleDataTransferMask = 0b1100'0000'0000;
	static const u32 armSingleDataTransferBits = 0b0100'0000'0000;
	static const u32 armBlockDataTransferMask = 0b1110'0000'0000;
	static const u32 armBlockDataTransferBits = 0b1000'0000'0000;
	static const u32 armBranchMask = 0b1110'0000'0000;
	static const u32 armBranchBits = 0b1010'0000'0000;
	static const u32 armCoprocessorDataTransferMask = 0b1110'0000'0000;
	static const u32 armCoprocessorDataTransferBits = 0b1100'0000'0000;
	static const u32 armCoprocessorDataOperationMask = 0b1111'0000'0001;
	static const u32 armCoprocessorDataOperationBits = 0b1110'0000'0000;
	static const u32 armCoprocessorRegisterTransferMask = 0b1111'0000'0001;
	static const u32 armCoprocessorRegisterTransferBits = 0b1110'0000'0001;
	static const u32 armSoftwareInterruptMask = 0b1111'0000'0000;
	static const u32 armSoftwareInterruptBits = 0b1111'0000'0000;
	static const u16 thumbMoveShiftedRegMask = 0b1110'0000'00;
	static const u16 thumbMoveShiftedRegBits = 0b0000'0000'00;
	static const u16 thumbAddSubtractMask = 0b1111'1000'00;
	static const u16 thumbAddSubtractBits = 0b0001'1000'00;
	static const u16 thumbAluImmediateMask = 0b1110'0000'00;
	static const u16 thumbAluImmediateBits = 0b0010'0000'00;
	static const u16 thumbAluRegMask = 0b1111'1100'00;
	static const u16 thumbAluRegBits = 0b0100'0000'00;
	static const u16 thumbHighRegOperationMask = 0b1111'1100'00;
	static const u16 thumbHighRegOperationBits = 0b0100'0100'00;
	static const u16 thumbPcRelativeLoadMask = 0b1111'1000'00;
	static const u16 thumbPcRelativeLoadBits = 0b0100'1000'00;
	static const u16 thumbLoadStoreRegOffsetMask = 0b1111'0010'00;
	static const u16 thumbLoadStoreRegOffsetBits = 0b0101'0000'00;
	static const u16 thumbLoadStoreSextMask = 0b1111'0010'00;
	static const u16 thumbLoadStoreSextBits = 0b0101'0010'00;
	static const u16 thumbLoadStoreImmediateOffsetMask = 0b1110'0000'00;
	static const u16 thumbLoadStoreImmediateOffsetBits = 0b0110'0000'00;
	static const u16 thumbLoadStoreHalfwordMask = 0b1111'0000'00;
	static const u16 thumbLoadStoreHalfwordBits = 0b1000'0000'00;
	static const u16 thumbSpRelativeLoadStoreMask = 0b1111'0000'00;
	static const u16 thumbSpRelativeLoadStoreBits = 0b1001'0000'00;
	static const u16 thumbLoadAddressMask = 0b1111'0000'00;
	static const u16 thumbLoadAddressBits = 0b1010'0000'00;
	static const u16 thumbSpAddOffsetMask = 0b1111'1111'00;
	static const u16 thumbSpAddOffsetBits = 0b1011'0000'00;
	static const u16 thumbPushPopRegistersMask = 0b1111'0110'00;
	static const u16 thumbPushPopRegistersBits = 0b1011'0100'00;
	static const u16 thumbMultipleLoadStoreMask = 0b1111'0000'00;
	static const u16 thumbMultipleLoadStoreBits = 0b1100'0000'00;
	static const u16 thumbConditionalBranchMask = 0b1111'0000'00;
	static const u16 thumbConditionalBranchBits = 0b1101'0000'00;
	static const u16 thumbUndefined1Mask = 0b1111'1111'00;
	static const u16 thumbUndefined1Bits = 0b1101'1110'00;
	static const u16 thumbSoftwareInterruptMask = 0b1111'1111'00;
	static const u16 thumbSoftwareInterruptBits = 0b1101'1111'00;
	static const u16 thumbUnconditionalBranchMask = 0b1111'1000'00;
	static const u16 thumbUnconditionalBranchBits = 0b1110'0000'00;
	static const u16 thumbUndefined2Mask = 0b1111'1000'00;
	static const u16 thumbUndefined2Bits = 0b1110'1000'00;
	static const u16 thumbLongBranchLinkMask = 0b1111'0000'00;
	static const u16 thumbLongBranchLinkBits = 0b1111'0000'00;

	using lutEntry = void (ARM7TDMI<T>::*)(u32);
	using thumbLutEntry = void (ARM7TDMI<T>::*)(u16);

	template <std::size_t lutFillIndex>
	constexpr static lutEntry decode() {
		if constexpr ((lutFillIndex & armUndefined1Mask) == armUndefined1Bits) {
			return &ARM7TDMI<T>::undefined;
		} else if constexpr ((lutFillIndex & armUndefined2Mask) == armUndefined2Bits) {
			return &ARM7TDMI<T>::undefined;
		} else if constexpr ((lutFillIndex & armUndefined3Mask) == armUndefined3Bits) {
			return &ARM7TDMI<T>::undefined;
		} else if constexpr ((lutFillIndex & armUndefined4Mask) == armUndefined4Bits) {
			return &ARM7TDMI<T>::undefined;
		} else if constexpr ((lutFillIndex & armMultiplyMask) == armMultiplyBits) {
			return &ARM7TDMI<T>::multiply<(bool)(lutFillIndex & 0b0000'0010'0000), (bool)(lutFillIndex & 0b0000'0001'0000)>;
		} else if constexpr ((lutFillIndex & armMultiplyLongMask) == armMultiplyLongBits) {
			return &ARM7TDMI<T>::multiplyLong<(bool)(lutFillIndex & 0b0000'0100'0000), (bool)(lutFillIndex & 0b0000'0010'0000), (bool)(lutFillIndex & 0b0000'0001'0000)>;
		} else if constexpr ((lutFillIndex & armPsrLoadMask) == armPsrLoadBits) {
			return &ARM7TDMI<T>::psrLoad<(bool)(lutFillIndex & 0b0000'0100'0000)>;
		} else if constexpr ((lutFillIndex & armPsrStoreRegMask) == armPsrStoreRegBits) {
			return &ARM7TDMI<T>::psrStoreReg<(bool)(lutFillIndex & 0b0000'0100'0000)>;
		} else if constexpr ((lutFillIndex & armPsrStoreImmediateMask) == armPsrStoreImmediateBits) {
			return &ARM7TDMI<T>::psrStoreImmediate<(bool)(lutFillIndex & 0b0000'0100'0000)>;
		} else if constexpr ((lutFillIndex & armSingleDataSwapMask) == armSingleDataSwapBits) {
			return &ARM7TDMI<T>::singleDataSwap<(bool)(lutFillIndex & 0b0000'0100'0000)>;
		} else if constexpr ((lutFillIndex & armBranchExchangeMask) == armBranchExchangeBits) {
			return &ARM7TDMI<T>::branchExchange;
		} else if constexpr ((lutFillIndex & armHalfwordDataTransferMask) == armHalfwordDataTransferBits) {
			return &ARM7TDMI<T>::halfwordDataTransfer<(bool)(lutFillIndex & 0b0001'0000'0000), (bool)(lutFillIndex & 0b0000'1000'0000), (bool)(lutFillIndex & 0b0000'0100'0000), (bool)(lutFillIndex & 0b0000'0010'0000), (bool)(lutFillIndex & 0b0000'0001'0000), ((lutFillIndex & 0b0000'0000'0110) >> 1)>;
		} else if constexpr ((lutFillIndex & armDataProcessingMask) == armDataProcessingBits) {
			return &ARM7TDMI<T>::dataProcessing<(bool)(lutFillIndex & 0b0010'0000'0000), ((lutFillIndex & 0b0001'1110'0000) >> 5), (bool)(lutFillIndex & 0b0000'0001'0000)>;
		} else if constexpr ((lutFillIndex & armSingleDataTransferMask) == armSingleDataTransferBits) {
			return &ARM7TDMI<T>::singleDataTransfer<(bool)(lutFillIndex & 0b0010'0000'0000), (bool)(lutFillIndex & 0b0001'0000'0000), (bool)(lutFillIndex & 0b0000'1000'0000), (bool)(lutFillIndex & 0b0000'0100'0000), (bool)(lutFillIndex & 0b0000'0010'0000), (bool)(lutFillIndex & 0b0000'0001'0000)>;
		} else if constexpr ((lutFillIndex & armBlockDataTransferMask) == armBlockDataTransferBits) {
			return &ARM7TDMI<T>::blockDataTransfer<(bool)(lutFillIndex & 0b0001'0000'0000), (bool)(lutFillIndex & 0b0000'1000'0000), (bool)(lutFillIndex & 0b0000'0100'0000), (bool)(lutFillIndex & 0b0000'0010'0000), (bool)(lutFillIndex & 0b0000'0001'0000)>;
		} else if constexpr ((lutFillIndex & armBranchMask) == armBranchBits) {
			return &ARM7TDMI<T>::branch<(bool)(lutFillIndex & 0b0001'0000'0000)>;
		} else if constexpr ((lutFillIndex & armCoprocessorDataTransferMask) == armCoprocessorDataTransferBits) {
			return &ARM7TDMI<T>::undefined;
		} else if constexpr ((lutFillIndex & armCoprocessorDataOperationMask) == armCoprocessorDataOperationBits) {
			return &ARM7TDMI<T>::undefined;
		} else if constexpr ((lutFillIndex & armCoprocessorRegisterTransferMask) == armCoprocessorRegisterTransferBits) {
			return &ARM7TDMI<T>::armCoprocessorRegisterTransfer<(bool)(lutFillIndex & 0b0000'0001'0000)>;
		} else if constexpr ((lutFillIndex & armSoftwareInterruptMask) == armSoftwareInterruptBits) {
			return &ARM7TDMI<T>::softwareInterrupt;
		}

		return &ARM7TDMI<T>::unknownOpcodeArm;
	}
	template <std::size_t... lutFillIndex>
	constexpr static std::array<lutEntry, 4096> generateTable(std::index_sequence<lutFillIndex...>) {
		return std::array{decode<lutFillIndex>()...};
	}
	constexpr static const std::array<lutEntry, 4096> armLUT = {
		generateTable(std::make_index_sequence<4096>())
	};

	template <std::size_t lutFillIndex>
	constexpr static thumbLutEntry decodeThumb() {
		if constexpr ((lutFillIndex & thumbAddSubtractMask) == thumbAddSubtractBits) {
			return &ARM7TDMI<T>::thumbAddSubtract<(bool)(lutFillIndex & 0b0000'0100'00), (bool)(lutFillIndex & 0b0000'0010'00), (lutFillIndex & 0b0000'0001'11)>;
		} else if constexpr ((lutFillIndex & thumbMoveShiftedRegMask) == thumbMoveShiftedRegBits) {
			return &ARM7TDMI<T>::thumbMoveShiftedReg<((lutFillIndex & 0b0001'1000'00) >> 5), (lutFillIndex & 0b0000'0111'11)>;
		} else if constexpr ((lutFillIndex & thumbAluImmediateMask) == thumbAluImmediateBits) {
			return &ARM7TDMI<T>::thumbAluImmediate<((lutFillIndex & 0b0001'1000'00) >> 5), ((lutFillIndex & 0b0000'0111'00) >> 2)>;
		} else if constexpr ((lutFillIndex & thumbAluRegMask) == thumbAluRegBits) {
			return &ARM7TDMI<T>::thumbAluReg<(lutFillIndex & 0b0000'0011'11)>;
		} else if constexpr ((lutFillIndex & thumbHighRegOperationMask) == thumbHighRegOperationBits) {
			return &ARM7TDMI<T>::thumbHighRegOperation<((lutFillIndex & 0b0000'0011'00) >> 2), (bool)(lutFillIndex & 0b0000'0000'10), (bool)(lutFillIndex & 0b0000'0000'01)>;
		} else if constexpr ((lutFillIndex & thumbPcRelativeLoadMask) == thumbPcRelativeLoadBits) {
			return &ARM7TDMI<T>::thumbPcRelativeLoad<((lutFillIndex & 0b0000'0111'00) >> 2)>;
		} else if constexpr ((lutFillIndex & thumbLoadStoreRegOffsetMask) == thumbLoadStoreRegOffsetBits) {
			return &ARM7TDMI<T>::thumbLoadStoreRegOffset<(bool)(lutFillIndex & 0b0000'1000'00), (bool)(lutFillIndex & 0b0000'0100'00), (lutFillIndex & 0b0000'0001'11)>;
		} else if constexpr ((lutFillIndex & thumbLoadStoreSextMask) == thumbLoadStoreSextBits) {
			return &ARM7TDMI<T>::thumbLoadStoreSext<((lutFillIndex & 0b0000'1100'00) >> 4), (lutFillIndex & 0b0000'0001'11)>;
		} else if constexpr ((lutFillIndex & thumbLoadStoreImmediateOffsetMask) == thumbLoadStoreImmediateOffsetBits) {
			return &ARM7TDMI<T>::thumbLoadStoreImmediateOffset<(bool)(lutFillIndex & 0b0001'0000'00), (bool)(lutFillIndex & 0b0000'1000'00), (lutFillIndex & 0b0000'0111'11)>;
		} else if constexpr ((lutFillIndex & thumbLoadStoreHalfwordMask) == thumbLoadStoreHalfwordBits) {
			return &ARM7TDMI<T>::thumbLoadStoreHalfword<(bool)(lutFillIndex & 0b0000'1000'00), (lutFillIndex & 0b0000'0111'11)>;
		} else if constexpr ((lutFillIndex & thumbSpRelativeLoadStoreMask) == thumbSpRelativeLoadStoreBits) {
			return &ARM7TDMI<T>::thumbSpRelativeLoadStore<(bool)(lutFillIndex & 0b0000'1000'00), ((lutFillIndex & 0b0000'0111'00) >> 2)>;
		} else if constexpr ((lutFillIndex & thumbLoadAddressMask) == thumbLoadAddressBits) {
			return &ARM7TDMI<T>::thumbLoadAddress<(bool)(lutFillIndex & 0b0000'1000'00), ((lutFillIndex & 0b0000'0111'00) >> 2)>;
		} else if constexpr ((lutFillIndex & thumbSpAddOffsetMask) == thumbSpAddOffsetBits) {
			return &ARM7TDMI<T>::thumbSpAddOffset<(bool)(lutFillIndex & 0b0000'0000'10)>;
		} else if constexpr ((lutFillIndex & thumbPushPopRegistersMask) == thumbPushPopRegistersBits) {
			return &ARM7TDMI<T>::thumbPushPopRegisters<(bool)(lutFillIndex & 0b0000'1000'00), (bool)(lutFillIndex & 0b0000'0001'00)>;
		} else if constexpr ((lutFillIndex & thumbMultipleLoadStoreMask) == thumbMultipleLoadStoreBits) {
			return &ARM7TDMI<T>::thumbMultipleLoadStore<(bool)(lutFillIndex & 0b0000'1000'00), ((lutFillIndex & 0b0000'0111'00) >> 2)>;
		} else if constexpr ((lutFillIndex & thumbUndefined1Mask) == thumbUndefined1Bits) {
			return &ARM7TDMI<T>::thumbUndefined;
		} else if constexpr ((lutFillIndex & thumbSoftwareInterruptMask) == thumbSoftwareInterruptBits) {
			return &ARM7TDMI<T>::thumbSoftwareInterrupt;
		} else if constexpr ((lutFillIndex & thumbConditionalBranchMask) == thumbConditionalBranchBits) {
			return &ARM7TDMI<T>::thumbConditionalBranch<((lutFillIndex & 0b0000'1111'00) >> 2)>;
		} else if constexpr ((lutFillIndex & thumbUnconditionalBranchMask) == thumbUnconditionalBranchBits) {
			return &ARM7TDMI<T>::thumbUnconditionalBranch;
		} else if constexpr ((lutFillIndex & thumbUndefined2Mask) == thumbUndefined2Bits) {
			return &ARM7TDMI<T>::thumbUndefined;
		} else if constexpr ((lutFillIndex & thumbLongBranchLinkMask) == thumbLongBranchLinkBits) {
			return &ARM7TDMI<T>::thumbLongBranchLink<(bool)(lutFillIndex & 0b0000'1000'00)>;
		}

		return &ARM7TDMI<T>::unknownOpcodeThumb;
	}
	template <std::size_t... lutFillIndex>
	constexpr static std::array<thumbLutEntry, 1024> generateTableThumb(std::index_sequence<lutFillIndex...>) {
		return std::array{decodeThumb<lutFillIndex>()...};
	}
	constexpr static const std::array<thumbLutEntry, 1024> thumbLUT = {
		generateTableThumb(std::make_index_sequence<1024>())
	};
};
