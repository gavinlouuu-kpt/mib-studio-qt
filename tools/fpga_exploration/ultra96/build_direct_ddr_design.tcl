# Build the Ultra96 4-PPC/250-MHz direct-DDR image pipeline.
#
# Environment overrides:
#   MIB_ULTRA96_BUILD_ROOT  generated Vivado project and reports
#   MIB_HLS_IP_DIR          exported mib_mask_accel HLS IP repository
#   MIB_VALIDATE_ONLY       stop after block-design validation when nonzero
#   MIB_VIVADO_JOBS         implementation worker count (default 12)

set build_root /mnt/fpga-workspace/builds/mib-fpga-direct-ddr/ultra96-ppc4-250
set hls_ip_dir /mnt/fpga-workspace/builds/mib-fpga-ppc-clock-matrix/hls_ppc4/solution_250mhz/impl/ip
set validate_only 0
set implementation_jobs 12

if {[info exists ::env(MIB_ULTRA96_BUILD_ROOT)]} {
    set build_root $::env(MIB_ULTRA96_BUILD_ROOT)
}
if {[info exists ::env(MIB_HLS_IP_DIR)]} {
    set hls_ip_dir $::env(MIB_HLS_IP_DIR)
}
if {[info exists ::env(MIB_VALIDATE_ONLY)]} {
    set validate_only $::env(MIB_VALIDATE_ONLY)
}
if {[info exists ::env(MIB_VIVADO_JOBS)]} {
    set implementation_jobs $::env(MIB_VIVADO_JOBS)
}
if {![file exists [file join $hls_ip_dir component.xml]]} {
    error "HLS IP repository is missing component.xml: $hls_ip_dir"
}
if {![string is integer -strict $implementation_jobs] || $implementation_jobs < 1} {
    error "MIB_VIVADO_JOBS must be a positive integer"
}

puts "ULTRA96_DIRECT_DDR_CONFIG clock_mhz=250 ppc=4 hls_ip=$hls_ip_dir jobs=$implementation_jobs"
file mkdir $build_root
create_project -force mib_ultra96_ddr $build_root -part xczu3eg-sbva484-1-e
set_property target_language Verilog [current_project]
set_property ip_repo_paths [file normalize $hls_ip_dir] [current_project]
update_ip_catalog

create_bd_design mib_ultra96_ddr

set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:* ps]
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP1 {0} \
    CONFIG.PSU__USE__M_AXI_GP2 {0} \
    CONFIG.PSU__MAXIGP0__DATA_WIDTH {128} \
    CONFIG.PSU__USE__S_AXI_GP2 {1} \
    CONFIG.PSU__SAXIGP2__DATA_WIDTH {128} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100}] $ps

set accel [create_bd_cell -type ip -vlnv xilinx.com:hls:mib_mask_accel:1.0 accel]

# The host path performs only low-rate AXI-Lite register access.
set control_interconnect [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:* control_interconnect]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] $control_interconnect

# The data path carries two read masters and one write master directly between
# the accelerator and the PS high-performance DDR port.
set ddr_interconnect [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:* ddr_interconnect]
set_property -dict [list CONFIG.NUM_SI {3} CONFIG.NUM_MI {1}] $ddr_interconnect

set reset [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:* reset]
set clock_generator [create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:* clock_generator]
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ {99.999001} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {250} \
    CONFIG.USE_LOCKED {true} \
    CONFIG.USE_RESET {true} \
    CONFIG.RESET_TYPE {ACTIVE_LOW}] $clock_generator

connect_bd_intf_net [get_bd_intf_pins ps/M_AXI_HPM0_FPD] \
                    [get_bd_intf_pins control_interconnect/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins control_interconnect/M00_AXI] \
                    [get_bd_intf_pins accel/s_axi_control]

connect_bd_intf_net [get_bd_intf_pins accel/m_axi_gmem0] \
                    [get_bd_intf_pins ddr_interconnect/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins accel/m_axi_gmem1] \
                    [get_bd_intf_pins ddr_interconnect/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins accel/m_axi_gmem2] \
                    [get_bd_intf_pins ddr_interconnect/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins ddr_interconnect/M00_AXI] \
                    [get_bd_intf_pins ps/S_AXI_HP0_FPD]

connect_bd_net [get_bd_pins ps/pl_clk0] [get_bd_pins clock_generator/clk_in1]
connect_bd_net [get_bd_pins ps/pl_resetn0] [get_bd_pins clock_generator/resetn]
connect_bd_net [get_bd_pins clock_generator/locked] [get_bd_pins reset/dcm_locked]
set fabric_clock [get_bd_pins clock_generator/clk_out1]

connect_bd_net $fabric_clock \
    [get_bd_pins ps/maxihpm0_fpd_aclk] \
    [get_bd_pins ps/saxihp0_fpd_aclk] \
    [get_bd_pins accel/ap_clk] \
    [get_bd_pins control_interconnect/aclk] \
    [get_bd_pins ddr_interconnect/aclk] \
    [get_bd_pins reset/slowest_sync_clk]

connect_bd_net [get_bd_pins ps/pl_resetn0] [get_bd_pins reset/ext_reset_in]
connect_bd_net [get_bd_pins reset/peripheral_aresetn] \
    [get_bd_pins accel/ap_rst_n] \
    [get_bd_pins control_interconnect/aresetn] \
    [get_bd_pins ddr_interconnect/aresetn]

proc only_object {objects description} {
    if {[llength $objects] != 1} {
        error "Expected exactly one $description, got: $objects"
    }
    return [lindex $objects 0]
}

set ps_space [only_object \
    [get_bd_addr_spaces -of_objects [get_bd_intf_pins ps/M_AXI_HPM0_FPD]] \
    "PS HPM0 address space"]
set input_space [only_object \
    [get_bd_addr_spaces -of_objects [get_bd_intf_pins accel/m_axi_gmem0]] \
    "accelerator input address space"]
set background_space [only_object \
    [get_bd_addr_spaces -of_objects [get_bd_intf_pins accel/m_axi_gmem1]] \
    "accelerator background address space"]
set output_space [only_object \
    [get_bd_addr_spaces -of_objects [get_bd_intf_pins accel/m_axi_gmem2]] \
    "accelerator output address space"]
set control_segment [only_object \
    [get_bd_addr_segs accel/s_axi_control/Reg] \
    "accelerator control segment"]
set ddr_segment [only_object \
    [get_bd_addr_segs ps/SAXIGP2/HP0_DDR_LOW] \
    "PS HP0 low-DDR segment"]

assign_bd_address -offset 0xA0030000 -range 0x00010000 \
    -target_address_space $ps_space $control_segment -force
foreach accelerator_space [list $input_space $background_space $output_space] {
    assign_bd_address -offset 0x00000000 -range 0x80000000 \
        -target_address_space $accelerator_space $ddr_segment -force
}

validate_bd_design
save_bd_design

if {$validate_only} {
    puts "ULTRA96_DIRECT_DDR_VALIDATE_COMPLETE $build_root"
    exit
}

generate_target all [get_files mib_ultra96_ddr.bd]
set wrapper [make_wrapper -files [get_files mib_ultra96_ddr.bd] -top]
add_files -norecurse $wrapper
set_property top mib_ultra96_ddr_wrapper [current_fileset]
update_compile_order -fileset sources_1
set_property STEPS.WRITE_BITSTREAM.ARGS.BIN_FILE true [get_runs impl_1]

launch_runs impl_1 -to_step write_bitstream -jobs $implementation_jobs
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    error "Implementation did not complete: [get_property STATUS [get_runs impl_1]]"
}

open_run impl_1
report_utilization -file [file join $build_root utilization.rpt]
report_timing_summary -file [file join $build_root timing_summary.rpt]
report_clocks -file [file join $build_root clocks.rpt]
check_timing -verbose -file [file join $build_root check_timing.rpt]

set setup_path [get_timing_paths -delay_type max -max_paths 1 -nworst 1]
set hold_path [get_timing_paths -delay_type min -max_paths 1 -nworst 1]
if {[llength $setup_path] != 1 || [llength $hold_path] != 1} {
    error "Implementation did not produce both setup and hold timing paths"
}
set worst_setup_slack [get_property SLACK $setup_path]
set worst_hold_slack [get_property SLACK $hold_path]
set timing_gate [open [file join $build_root timing_gate.txt] w]
puts $timing_gate "clock_mhz=250"
puts $timing_gate "pixels_per_clock=4"
puts $timing_gate "worst_setup_slack_ns=$worst_setup_slack"
puts $timing_gate "worst_hold_slack_ns=$worst_hold_slack"
close $timing_gate
puts "ULTRA96_DIRECT_DDR_TIMING setup_slack_ns=$worst_setup_slack hold_slack_ns=$worst_hold_slack"
if {$worst_setup_slack < 0.0 || $worst_hold_slack < 0.0} {
    error "Implementation failed timing: setup slack $worst_setup_slack ns, hold slack $worst_hold_slack ns"
}

write_hw_platform -fixed -include_bit -force \
    [file join $build_root mib_ultra96_ddr.xsa]
set run_dir [get_property DIRECTORY [get_runs impl_1]]
foreach extension {bit bin} {
    set generated [file join $run_dir "mib_ultra96_ddr_wrapper.$extension"]
    if {[file exists $generated]} {
        file copy -force $generated \
            [file join $build_root "mib_ultra96_ddr_wrapper.$extension"]
    }
}
puts "ULTRA96_DIRECT_DDR_BUILD_COMPLETE $build_root"
