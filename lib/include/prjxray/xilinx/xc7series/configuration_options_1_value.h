/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef PRJXRAY_LIB_XILINX_XC7SERIES_CONFIGURATION_OPTIONS_1_VALUE_H
#define PRJXRAY_LIB_XILINX_XC7SERIES_CONFIGURATION_OPTIONS_1_VALUE_H

#include <prjxray/bit_ops.h>
#include <prjxray/xilinx/configuration_packet.h>
#include <prjxray/xilinx/configuration_register.h>

namespace prjxray {
namespace xilinx {
namespace xc7series {

// COR1, register 01110, UG470 (v1.10) Table 5-32 and Table 5-33.
//
// SIX FIELDS ARE DOCUMENTED AND THIS CLASS CARRIES ALL SIX. Every remaining
// bit is Reserved with a documented value of 0, which is this class's initial
// state, so a default-constructed value is the 0x00000000 the writer has
// always emitted for Series7 and the 0x00000000 in every Vivado full-bitstream
// packet dump the xc7-dpr toolchain repository has captured -- 20 of 20 files
// under gateware/vivado-dfx-golden*/, all reading `COR1 <= 0x00000000`.
//
// THE TWO FIELDS THE SEU WORK NEEDS ARE RBCRC_EN AND RBCRC_ACTION. Bit 8
// turns on the continuous background readback of configuration memory
// (UG470 ch. 8); bits 16:15 say what the engine does when it finds a
// mismatch. The other four are here because a builder that offers only the
// two a caller happens to want is how the next caller ends up writing a raw
// hex word again.
//
// THE ACTION ENCODING IS NOT MONOTONIC and it is the one place a
// transcription can go wrong silently: Table 5-33 gives 00 Continue,
// 01 Halt, 11 Correct_And_Halt, 10 Correct_And_Continue -- so 0b10 is
// CORRECT_AND_CONTINUE and 0b11 is CORRECT_AND_HALT, not the other way
// round. Independently transcribed in scripts/bit-packets.py's RBCRC_ACTION
// table in the xc7-dpr toolchain repository, which agrees.
class ConfigurationOptions1Value {
       public:
	enum class ReadbackCrcAction : uint32_t {
		Continue = 0x0,
		Halt = 0x1,
		CorrectAndContinue = 0x2,
		CorrectAndHalt = 0x3,
	};

	enum class BpiFirstReadCycle : uint32_t {
		Cclk1 = 0x0,
		Cclk2 = 0x1,
		Cclk3 = 0x2,
		Cclk4 = 0x3,
	};

	enum class BpiPageSize : uint32_t {
		Bytes1 = 0x0,
		Bytes4 = 0x1,
		Bytes8 = 0x2,
	};

	ConfigurationOptions1Value() : value_(0) {}

	operator uint32_t() const { return value_; }

	// Bit 17. 0 (default) disables deassertion of PERSIST with DESYNC.
	ConfigurationOptions1Value& SetPersistDeassertAtDesync(bool enabled) {
		value_ = bit_field_set(value_, 17, 17, enabled ? 1 : 0);
		return *this;
	}

	// Bits 16:15.
	ConfigurationOptions1Value& SetReadbackCrcAction(
	    ReadbackCrcAction action) {
		value_ = bit_field_set(value_, 16, 15,
		                       static_cast<uint32_t>(action));
		return *this;
	}

	// Bit 9, RBCRC_NO_PIN, stated in the register's own polarity rather
	// than in the caller's: the documented bit means "no pin", so 1
	// DISABLES INIT_B as the readback-CRC error status output. A setter
	// named for the enable would have to invert here, and an inverted
	// bool is the defect this project has already paid for in CTL0[8]
	// GLUTMASK_B, whose name also reads backwards from its effect.
	ConfigurationOptions1Value& SetReadbackCrcErrorPinDisabled(
	    bool disabled) {
		value_ = bit_field_set(value_, 9, 9, disabled ? 1 : 0);
		return *this;
	}

	// Bit 8.
	ConfigurationOptions1Value& SetContinuousReadbackCrc(bool enabled) {
		value_ = bit_field_set(value_, 8, 8, enabled ? 1 : 0);
		return *this;
	}

	// Bits 3:2.
	ConfigurationOptions1Value& SetBpiFirstReadCycle(
	    BpiFirstReadCycle cycles) {
		value_ =
		    bit_field_set(value_, 3, 2, static_cast<uint32_t>(cycles));
		return *this;
	}

	// Bits 1:0. 0b11 is Reserved in Table 5-33 and has no enumerator.
	ConfigurationOptions1Value& SetBpiPageSize(BpiPageSize size) {
		value_ =
		    bit_field_set(value_, 1, 0, static_cast<uint32_t>(size));
		return *this;
	}

       private:
	uint32_t value_;
};

}  // namespace xc7series
}  // namespace xilinx
}  // namespace prjxray

#endif  // PRJXRAY_LIB_XILINX_XC7SERIES_CONFIGURATION_OPTIONS_1_VALUE_H
