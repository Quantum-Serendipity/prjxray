/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <prjxray/xilinx/xc7series/stream_replay.h>

#include <iomanip>
#include <sstream>

namespace prjxray {
namespace xilinx {
namespace xc7series {

namespace {

constexpr uint32_t kSyncWord = 0xAA995566;
constexpr size_t kWordsPerFrame = 101;
constexpr size_t kRowEndDummyPositions = 2;
constexpr size_t kMfwrWordsAfterBurst = 8;
constexpr size_t kMfwrWordsAfterFar = 4;

constexpr uint32_t kRegFar = 1;
constexpr uint32_t kRegFdri = 2;
constexpr uint32_t kRegCmd = 4;
constexpr uint32_t kRegMfwr = 10;
constexpr uint32_t kRegIdcode = 12;

constexpr uint32_t kCmdWcfg = 1;
constexpr uint32_t kCmdMfw = 2;
constexpr uint32_t kCmdDesync = 13;

constexpr uint32_t kOpNop = 0;
constexpr uint32_t kOpWrite = 2;

std::string Hex(uint32_t v) {
	std::ostringstream s;
	s << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
	return s.str();
}

bool SameGroup(const FrameAddress& a, const FrameAddress& b) {
	return a.block_type() == b.block_type() &&
	       a.is_bottom_half_rows() == b.is_bottom_half_rows() &&
	       a.row() == b.row();
}

struct Position {
	bool valid = false;
	uint32_t address = 0;
	size_t dummy = 0;
	bool past_end = false;
	uint32_t resume = 0;
};

class Replayer {
       public:
	Replayer(const Part& part, StreamReplay* out)
	    : part_(part), out_(out) {}

	bool Write(uint32_t reg, const std::vector<uint32_t>& payload) {
		if (expect_mfwr_ && reg != kRegMfwr)
			return Fail(
			    "a FAR write executed under CMD:MFW is not "
			    "followed "
			    "by an MFWR write, so it replays the frame buffer "
			    "into " +
			    Hex(position_.address) +
			    " as a frame write of its own");
		switch (reg) {
			case kRegFar:
				return WriteFar(payload);
			case kRegFdri:
				return WriteFdri(payload);
			case kRegCmd:
				return WriteCmd(payload);
			case kRegMfwr:
				return WriteMfwr(payload);
			case kRegIdcode:
				if (payload.size() != 1)
					return Fail(
					    "an IDCODE write is not one word");
				if (payload[0] != part_.idcode())
					return Fail(
					    "the stream writes IDCODE " +
					    Hex(payload[0]) +
					    " and the part's is " +
					    Hex(part_.idcode()));
				return true;
			default:
				return true;
		}
	}

	bool Finish() {
		if (expect_mfwr_)
			return Fail(
			    "the stream ends after a FAR write under CMD:MFW "
			    "with no MFWR");
		DropPending();
		return error_.empty();
	}

	bool desynced() const { return desynced_; }
	const std::string& error() const { return error_; }

       private:
	bool Fail(const std::string& message) {
		if (error_.empty())
			error_ = message;
		return false;
	}

	void Commit(const Position& at, const std::vector<uint32_t>& words) {
		if (at.dummy)
			++out_->dummy_frames;
		else
			out_->committed.push_back({at.address, words});
	}

	void DropPending() {
		if (!pending_)
			return;
		if (pending_at_.dummy)
			++out_->dummy_frames;
		else
			out_->discarded.push_back(
			    {pending_at_.address, frame_buffer_});
		pending_ = false;
	}

	Position Advance(Position p) {
		if (p.dummy == 0) {
			auto next =
			    part_.GetNextFrameAddress(FrameAddress(p.address));
			if (!next) {
				p.past_end = true;
				p.dummy = 1;
			} else if (!SameGroup(FrameAddress(p.address), *next)) {
				p.resume = static_cast<uint32_t>(*next);
				p.dummy = 1;
			} else {
				p.address = static_cast<uint32_t>(*next);
			}
			return p;
		}
		if (p.past_end || p.dummy < kRowEndDummyPositions) {
			++p.dummy;
			return p;
		}
		p.address = p.resume;
		p.dummy = 0;
		return p;
	}

	bool WriteFar(const std::vector<uint32_t>& payload) {
		if (payload.size() != 1)
			return Fail("a FAR write is not one word");
		++out_->far_writes;
		DropPending();
		position_ = Position();
		position_.address = payload[0];
		position_.valid =
		    part_.IsValidFrameAddress(FrameAddress(payload[0]));
		if (cmd_ != kCmdMfw)
			return true;
		if (!position_.valid)
			return Fail(
			    "a FAR write under CMD:MFW names " +
			    Hex(payload[0]) +
			    ", which is not a frame address of the part");
		if (frame_buffer_.empty())
			return Fail(
			    "a FAR write under CMD:MFW with no frame in the "
			    "frame buffer");
		Commit(position_, frame_buffer_);
		++out_->mfw_replays;
		expect_mfwr_ = true;
		return true;
	}

	bool WriteFdri(const std::vector<uint32_t>& payload) {
		if (payload.empty())
			return true;
		if (cmd_ != kCmdWcfg)
			return Fail("an FDRI write while CMD holds " +
			            std::to_string(cmd_) + ", not WCFG");
		if (payload.size() % kWordsPerFrame != 0)
			return Fail("an FDRI write of " +
			            std::to_string(payload.size()) +
			            " words is not a whole number of frames");
		for (size_t f = 0; f < payload.size(); f += kWordsPerFrame) {
			std::vector<uint32_t> frame(
			    payload.begin() + f,
			    payload.begin() + f + kWordsPerFrame);
			if (pending_) {
				Commit(pending_at_, frame_buffer_);
				pending_at_ = Advance(pending_at_);
			} else {
				if (!position_.valid)
					return Fail(
					    "an FDRI write with no valid frame "
					    "address in FAR (" +
					    Hex(position_.address) + ")");
				pending_at_ = position_;
			}
			frame_buffer_ = std::move(frame);
			pending_ = true;
		}
		return true;
	}

	bool WriteCmd(const std::vector<uint32_t>& payload) {
		if (payload.size() != 1)
			return Fail("a CMD write is not one word");
		cmd_ = payload[0] & 0x1f;
		if (cmd_ == kCmdDesync) {
			DropPending();
			desynced_ = true;
		}
		return true;
	}

	bool WriteMfwr(const std::vector<uint32_t>& payload) {
		if (cmd_ != kCmdMfw)
			return Fail("an MFWR write while CMD holds " +
			            std::to_string(cmd_) + ", not MFW");
		for (uint32_t w : payload)
			if (w)
				return Fail(
				    "an MFWR write carries a non-zero word");
		if (expect_mfwr_) {
			expect_mfwr_ = false;
			if (payload.size() != kMfwrWordsAfterFar)
				return Fail(
				    "an MFWR write after a FAR carries " +
				    std::to_string(payload.size()) +
				    " words, not " +
				    std::to_string(kMfwrWordsAfterFar));
			return true;
		}
		if (payload.size() != kMfwrWordsAfterBurst)
			return Fail(
			    "an MFWR write with no FAR before it carries " +
			    std::to_string(payload.size()) + " words, not " +
			    std::to_string(kMfwrWordsAfterBurst));
		if (!pending_)
			return Fail(
			    "an MFWR write with no FAR before it and no "
			    "pending "
			    "FDRI frame to commit");
		Commit(pending_at_, frame_buffer_);
		pending_ = false;
		++out_->mfw_replays;
		return true;
	}

	const Part& part_;
	StreamReplay* out_;
	std::string error_;
	uint32_t cmd_ = 0;
	Position position_;
	Position pending_at_;
	bool pending_ = false;
	std::vector<uint32_t> frame_buffer_;
	bool expect_mfwr_ = false;
	bool desynced_ = false;
};

}  // namespace

bool ReplayConfigurationStream(const std::vector<uint8_t>& bytes,
                               const Part& part,
                               StreamReplay* out,
                               std::string* error) {
	*out = StreamReplay();
	size_t start = bytes.size();
	for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
		uint32_t w = (uint32_t(bytes[i]) << 24) |
		             (uint32_t(bytes[i + 1]) << 16) |
		             (uint32_t(bytes[i + 2]) << 8) | bytes[i + 3];
		if (w == kSyncWord) {
			start = i + 4;
			break;
		}
	}
	if (start == bytes.size()) {
		*error = "no sync word";
		return false;
	}
	if ((bytes.size() - start) % 4 != 0) {
		*error =
		    "the stream after the sync word is not a whole number "
		    "of 32-bit words";
		return false;
	}
	std::vector<uint32_t> words;
	for (size_t i = start; i < bytes.size(); i += 4)
		words.push_back((uint32_t(bytes[i]) << 24) |
		                (uint32_t(bytes[i + 1]) << 16) |
		                (uint32_t(bytes[i + 2]) << 8) | bytes[i + 3]);

	Replayer replayer(part, out);
	bool have_type1 = false;
	uint32_t last_reg = 0;
	size_t i = 0;
	while (i < words.size() && !replayer.desynced()) {
		const uint32_t header = words[i++];
		const uint32_t type = header >> 29;
		const uint32_t op = (header >> 27) & 0x3;
		size_t count;
		uint32_t reg;
		if (type == 1) {
			reg = (header >> 13) & 0x3fff;
			count = header & 0x7ff;
			if (op == kOpNop) {
				i += count;
				continue;
			}
			last_reg = reg;
			have_type1 = true;
		} else if (type == 2) {
			if (!have_type1) {
				*error =
				    "a type-2 packet with no type-1 packet "
				    "before it";
				return false;
			}
			reg = last_reg;
			count = header & 0x7ffffff;
		} else {
			std::ostringstream err;
			err << "word " << i - 1 << " after the sync word, "
			    << Hex(header) << ", is not a packet header";
			*error = err.str();
			return false;
		}
		if (count > words.size() - i) {
			*error = "a packet runs past the end of the stream";
			return false;
		}
		if (op == kOpWrite) {
			std::vector<uint32_t> payload(
			    words.begin() + i, words.begin() + i + count);
			if (!replayer.Write(reg, payload)) {
				*error = replayer.error();
				return false;
			}
		}
		i += count;
	}
	if (!replayer.Finish()) {
		*error = replayer.error();
		return false;
	}
	out->desynced = replayer.desynced();
	return true;
}

}  // namespace xc7series
}  // namespace xilinx
}  // namespace prjxray
