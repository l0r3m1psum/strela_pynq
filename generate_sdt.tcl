
if { $argc != 2 } {
    puts "Error: Incorrect number of arguments."
    puts "Usage: xsct generate_sdt.tcl <path_to_xsa> <output_directory>"
    exit 1
}

set xsa_file [lindex $argv 0]
set output_dir [lindex $argv 1]

sdtgen set_dt_param -dir $output_dir -xsa $xsa_file

# sdtgen set_dt_param -board_dts zcu102-rev1.0
# sdtgen set_dt_param -user_dts path/to/system-user.dtsi
# sdtgen set_dt_param -trace enable -debug enable

sdtgen generate_sdt
