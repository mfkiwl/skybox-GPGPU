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

`include "VX_define.vh"

module VX_execute import VX_gpu_pkg::*; #(
    parameter `STRING INSTANCE_ID = "",
    parameter CORE_ID = 0
) (
    `SCOPE_IO_DECL

    input wire              clk,
    input wire              reset,

`ifdef PERF_ENABLE
    VX_mem_perf_if.slave    mem_perf_if,
    VX_pipeline_perf_if.slave pipeline_perf_if,
`endif

    input base_dcrs_t       base_dcrs,

    // Dcache interface
    VX_lsu_mem_if.master    lsu_mem_if [`NUM_LSU_BLOCKS],

    // dispatch interface
    VX_dispatch_if.slave    dispatch_if [`NUM_EX_UNITS * `ISSUE_WIDTH],

    // commit interface
    VX_commit_if.master     commit_if [`NUM_EX_UNITS * `ISSUE_WIDTH],

    // scheduler interfaces
    VX_sched_csr_if.slave   sched_csr_if,
    VX_branch_ctl_if.master branch_ctl_if [`NUM_ALU_BLOCKS],
    VX_warp_ctl_if.master   warp_ctl_if,

`ifdef EXT_TEX_ENABLE
    VX_tex_bus_if.master    tex_bus_if,
`ifdef PERF_ENABLE
    VX_tex_perf_if.slave    perf_tex_if,
`endif
`endif

`ifdef EXT_RASTER_ENABLE
    VX_raster_bus_if.slave  raster_bus_if,
`ifdef PERF_ENABLE
    VX_raster_perf_if.slave perf_raster_if,
`endif
`endif

`ifdef EXT_OM_ENABLE
    VX_om_bus_if.master     om_bus_if,
`ifdef PERF_ENABLE
    VX_om_perf_if.slave     perf_om_if,
`endif
`endif

    // commit interface
    VX_commit_csr_if.slave  commit_csr_if
);

`ifdef EXT_F_ENABLE
    VX_fpu_csr_if fpu_csr_if[`NUM_FPU_BLOCKS]();
`endif

    `RESET_RELAY (alu_reset, reset);
    `RESET_RELAY (lsu_reset, reset);
    `RESET_RELAY (sfu_reset, reset);

    VX_alu_unit #(
        .INSTANCE_ID ($sformatf("%s-alu", INSTANCE_ID))
    ) alu_unit (
        .clk            (clk),
        .reset          (alu_reset),
        .dispatch_if    (dispatch_if[`EX_ALU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
        .commit_if      (commit_if[`EX_ALU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
        .branch_ctl_if  (branch_ctl_if)
    );

    `SCOPE_IO_SWITCH (1)

    VX_lsu_unit #(
        .INSTANCE_ID ($sformatf("%s-lsu", INSTANCE_ID))
    ) lsu_unit (
        `SCOPE_IO_BIND  (0)
        .clk            (clk),
        .reset          (lsu_reset),
        .dispatch_if    (dispatch_if[`EX_LSU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
        .commit_if      (commit_if[`EX_LSU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
        .lsu_mem_if     (lsu_mem_if)
    );

`ifdef EXT_F_ENABLE
    `RESET_RELAY (fpu_reset, reset);

    VX_fpu_unit #(
        .INSTANCE_ID ($sformatf("%s-fpu", INSTANCE_ID))
    ) fpu_unit (
        .clk            (clk),
        .reset          (fpu_reset),
        .dispatch_if    (dispatch_if[`EX_FPU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
        .commit_if      (commit_if[`EX_FPU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
        .fpu_csr_if     (fpu_csr_if)
    );
`endif

    VX_sfu_unit #(
        .INSTANCE_ID ($sformatf("%s-sfu", INSTANCE_ID)),
        .CORE_ID (CORE_ID)
    ) sfu_unit (
        .clk            (clk),
        .reset          (sfu_reset),
    `ifdef PERF_ENABLE
        .mem_perf_if    (mem_perf_if),
        .pipeline_perf_if (pipeline_perf_if),
    `endif
        .base_dcrs      (base_dcrs),
        .dispatch_if    (dispatch_if[`EX_SFU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
        .commit_if      (commit_if[`EX_SFU * `ISSUE_WIDTH +: `ISSUE_WIDTH]),
    `ifdef EXT_F_ENABLE
        .fpu_csr_if     (fpu_csr_if),
    `endif

    `ifdef EXT_TEX_ENABLE
        .tex_bus_if     (tex_bus_if),
    `ifdef PERF_ENABLE
        .perf_tex_if    (perf_tex_if),
    `endif
    `endif

    `ifdef EXT_RASTER_ENABLE
        .raster_bus_if  (raster_bus_if),
    `ifdef PERF_ENABLE
        .perf_raster_if (perf_raster_if),
    `endif
    `endif

    `ifdef EXT_OM_ENABLE
        .om_bus_if      (om_bus_if),
    `ifdef PERF_ENABLE
        .perf_om_if     (perf_om_if),
    `endif
    `endif

        .commit_csr_if  (commit_csr_if),
        .sched_csr_if   (sched_csr_if),
        .warp_ctl_if    (warp_ctl_if)
    );

endmodule
