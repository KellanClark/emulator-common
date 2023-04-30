#pragma once

#include "../types.hpp"

class SystemControlCoprocessor {
public:
	u8 *dtcm;
	u8 *itcm;

	SystemControlCoprocessor() {
		dtcm = new u8[0x4000]; // 16KB
		itcm = new u8[0x8000]; // 32KB
	}

	void reset() {
		memset(dtcm, 0, 0x4000);
		memset(itcm, 0, 0x8000);

		control = 0x00012078;
		halted = false;
	}

	~SystemControlCoprocessor() {
		delete[] dtcm;
		delete[] itcm;
	}

	// Registers
	union {
		struct {
			u32 puEnable : 1;
			u32 alignmentCheck : 1;
			u32 dataCacheEnable : 1; // TODO
			u32 writeBufferEnable : 1;
			u32 : 3; // ARMv3 stuff
			u32 bigEndian : 1; // TODO
			u32 systemProtection : 1;
			u32 romProtection : 1;
			u32 : 1; // Implementation defined
			u32 branchPrediction : 1;
			u32 instructionCacheEnable : 1; // TODO
			u32 vectorOffset : 1;
			u32 cacheReplacement : 1; // TODO
			u32 preArmv5Mode : 1; // TODO
			u32 dtcmEnable : 1;
			u32 dtcmWriteOnly : 1;
			u32 itcmEnable : 1;
			u32 itcmWriteOnly : 1;
			u32 : 12;
		};
		u32 control; // c1,c0,0
	};

	union {
		struct {
			u32 : 1;
			u32 dtcmVirtualSize : 5;
			u32 : 6;
			u32 dtcmRegionBase : 20;
		};
		u32 dtcmConfig; // c9,c1,0
	};

	union {
		struct {
			u32 : 1;
			u32 itcmVirtualSize : 5;
			u32 : 6;
			u32 itcmRegionBase : 20;
		};
		u32 itcmConfig; // c9,c1,1
	};

	u32 dtcmStart;
	u32 dtcmEnd;
	u32 itcmEnd;

	bool halted;
};
