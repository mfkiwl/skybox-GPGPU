// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "func_unit.h"
#include <iostream>
#include <iomanip>
#include <string.h>
#include <assert.h>
#include <util.h>
#include "debug.h"
#include "core.h"
#include "constants.h"
#include "cache_sim.h"

using namespace vortex;

AluUnit::AluUnit(const SimContext& ctx, Core* core) : FuncUnit(ctx, core, "alu-unit") {}

void AluUnit::tick() {
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
		auto& input = Inputs.at(iw);
		if (input.empty())
			continue;
		auto& output = Outputs.at(iw);
		auto trace = input.front();
		int delay = 2;
		switch (trace->alu_type) {
		case AluType::ARITH:
		case AluType::BRANCH:
		case AluType::SYSCALL:
			output.push(trace, 2+delay);
			break;
		case AluType::IMUL:
			output.push(trace, LATENCY_IMUL+delay);
			break;
		case AluType::IDIV:
			output.push(trace, XLEN+delay);
			break;
		default:
			std::abort();
		}
		DT(3, this->name() << ": op" << trace->alu_type << ", " << *trace);
		if (trace->eop && trace->fetch_stall) {
			core_->resume(trace->wid);
		}
		input.pop();
	}
}

///////////////////////////////////////////////////////////////////////////////

FpuUnit::FpuUnit(const SimContext& ctx, Core* core) : FuncUnit(ctx, core, "fpu-unit") {}

void FpuUnit::tick() {
	for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
		auto& input = Inputs.at(iw);
		if (input.empty())
			continue;
		auto& output = Outputs.at(iw);
		auto trace = input.front();
		int delay = 2;
		switch (trace->fpu_type) {
		case FpuType::FNCP:
			output.push(trace, 2+delay);
			break;
		case FpuType::FMA:
			output.push(trace, LATENCY_FMA+delay);
			break;
		case FpuType::FDIV:
			output.push(trace, LATENCY_FDIV+delay);
			break;
		case FpuType::FSQRT:
			output.push(trace, LATENCY_FSQRT+delay);
			break;
		case FpuType::FCVT:
			output.push(trace, LATENCY_FCVT+delay);
			break;
		default:
			std::abort();
		}
		DT(3,this->name() << ": op=" << trace->fpu_type << ", " << *trace);
		input.pop();
	}
}

///////////////////////////////////////////////////////////////////////////////

LsuUnit::LsuUnit(const SimContext& ctx, Core* core)
	: FuncUnit(ctx, core, "lsu-unit")
	, pending_loads_(0)
{}

LsuUnit::~LsuUnit()
{}

void LsuUnit::reset() {
	for (auto& state : states_) {
		state.clear();
	}
	pending_loads_ = 0;
}

void LsuUnit::tick() {
	core_->perf_stats_.load_latency += pending_loads_;

	// handle memory responses
	for (uint32_t b = 0; b < NUM_LSU_BLOCKS; ++b) {
		auto& lsu_rsp_port = core_->lsu_demux_.at(b)->RspIn;
		if (lsu_rsp_port.empty())
			continue;
		auto& state = states_.at(b);
		auto& lsu_rsp = lsu_rsp_port.front();
		DT(3, this->name() << "-" << lsu_rsp);
		auto& entry = state.pending_rd_reqs.at(lsu_rsp.tag);
		auto trace = entry.trace;
		assert(!entry.mask.none());
		entry.mask &= ~lsu_rsp.mask; // track remaining
		if (entry.mask.none()) {
			// whole response received, release trace
			int iw = trace->wid % ISSUE_WIDTH;
			Outputs.at(iw).push(trace, 1);
			state.pending_rd_reqs.release(lsu_rsp.tag);
		}
		pending_loads_ -= lsu_rsp.mask.count();
		lsu_rsp_port.pop();
	}

	// handle LSU requests
	for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
		uint32_t block_idx = iw % NUM_LSU_BLOCKS;
		auto& state = states_.at(block_idx);
		if (state.fence_lock) {
			// wait for all pending memory operations to complete
			if (!state.pending_rd_reqs.empty())
				continue;
			Outputs.at(iw).push(state.fence_trace, 1);
			state.fence_lock = false;
			DT(3, this->name() << "-fence-unlock: " << state.fence_trace);
		}

		// check input queue
		auto& input = Inputs.at(iw);
		if (input.empty())
			continue;

		auto trace = input.front();

		if (trace->lsu_type == LsuType::FENCE) {
			// schedule fence lock
			state.fence_trace = trace;
			state.fence_lock = true;
			DT(3, this->name() << "-fence-lock: " << *trace);
			// remove input
			input.pop();
			continue;
		}

		bool is_write = (trace->lsu_type == LsuType::STORE);

		// check pending queue capacity
		if (!is_write && state.pending_rd_reqs.full()) {
			if (!trace->log_once(true)) {
				DT(4, "*** " << this->name() << "-queue-full: " << *trace);
			}
			continue;
		} else {
			trace->log_once(false);
		}

		// build memory request
		LsuReq lsu_req(NUM_LSU_LANES);
		lsu_req.write = is_write;
		{
			auto trace_data = std::dynamic_pointer_cast<LsuTraceData>(trace->data);
			auto t0 = trace->pid * NUM_LSU_LANES;
			for (uint32_t i = 0; i < NUM_LSU_LANES; ++i) {
				if (trace->tmask.test(t0 + i)) {
					lsu_req.mask.set(i);
					lsu_req.addrs.at(i) = trace_data->mem_addrs.at(t0 + i).addr;
				}
			}
		}
		uint32_t tag = 0;
		if (!is_write) {
			tag = state.pending_rd_reqs.allocate({trace, lsu_req.mask});
		}
		lsu_req.tag  = tag;
		lsu_req.cid  = trace->cid;
		lsu_req.uuid = trace->uuid;

		// send memory request
		core_->lsu_demux_.at(block_idx)->ReqIn.push(lsu_req);
		DT(3, this->name() << "-" << lsu_req);

		// update stats
		auto num_addrs = lsu_req.mask.count();
		if (is_write) {
			core_->perf_stats_.stores += num_addrs;
		} else {
			core_->perf_stats_.loads += num_addrs;
			pending_loads_ += num_addrs;
		}

		// do not wait on writes
		if (is_write) {
			Outputs.at(iw).push(trace, 1);
		}

		// remove input
		input.pop();
	}
}

///////////////////////////////////////////////////////////////////////////////

SfuUnit::SfuUnit(const SimContext& ctx, Core* core)
	: FuncUnit(ctx, core, "sfu-unit")
	, raster_units_(core->raster_units_)
	, tex_units_(core->tex_units_)
	, om_units_(core->om_units_)
	, input_idx_(0)
{
	for (auto& raster_unit : raster_units_) {
		pending_rsps_.push_back(&raster_unit->Output);
	}
	for (auto& tex_unit : tex_units_) {
		pending_rsps_.push_back(&tex_unit->Output);
	}
	for (auto& om_unit : om_units_) {
		pending_rsps_.push_back(&om_unit->Output);
	}
}

void SfuUnit::tick() {
	// handle pending responses
	for (auto pending_rsp : pending_rsps_) {
		if (pending_rsp->empty())
			continue;
		auto trace = pending_rsp->front();
		if (trace->cid != core_->id())
			continue;
		int iw = trace->wid % ISSUE_WIDTH;
		auto& output = Outputs.at(iw);
		output.push(trace, 1);
		pending_rsp->pop();
	}

	// check input queue
	for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
		auto& input = Inputs.at(iw);
		if (input.empty())
			continue;
		auto& output = Outputs.at(iw);
		auto trace = input.front();
		auto sfu_type = trace->sfu_type;
		bool release_warp = trace->fetch_stall;
		int delay = 2;
		switch  (sfu_type) {
		case SfuType::WSPAWN:
			output.push(trace, 2+delay);
			if (trace->eop) {
				auto trace_data = std::dynamic_pointer_cast<SFUTraceData>(trace->data);
				release_warp = core_->wspawn(trace_data->arg1, trace_data->arg2);
			}
			break;
		case SfuType::TMC:
		case SfuType::SPLIT:
		case SfuType::JOIN:
		case SfuType::PRED:
		case SfuType::CSRRW:
		case SfuType::CSRRS:
		case SfuType::CSRRC:
			output.push(trace, 2+delay);
			break;
		case SfuType::BAR: {
			output.push(trace, 2+delay);
			if (trace->eop) {
				auto trace_data = std::dynamic_pointer_cast<SFUTraceData>(trace->data);
				release_warp = core_->barrier(trace_data->arg1, trace_data->arg2, trace->wid);
			}
		} break;
		case SfuType::RASTER: {
			auto trace_data = std::dynamic_pointer_cast<RasterUnit::TraceData>(trace->data);
			raster_units_.at(trace_data->raster_idx)->Input.push(trace, delay);
		} break;
		case SfuType::OM: {
			auto trace_data = std::dynamic_pointer_cast<OMUnit::TraceData>(trace->data);
			om_units_.at(trace_data->om_idx)->Input.push(trace, delay);
		} break;
		case SfuType::TEX: {
			auto trace_data = std::dynamic_pointer_cast<TexUnit::TraceData>(trace->data);
			tex_units_.at(trace_data->tex_idx)->Input.push(trace, delay);
		} break;
		default:
			std::abort();
		}

		DT(3, this->name() << ": op=" << trace->sfu_type << ", " << *trace);
		if (trace->eop && release_warp)  {
			core_->resume(trace->wid);
		}

		input.pop();
	}
}