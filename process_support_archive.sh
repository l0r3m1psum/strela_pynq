#!/bin/sh

set -e

support_archive_name=$1

xsct tools/generate_sdt.tcl "${support_archive_name}.xsa" sdt_out
# sed '1i /dts-v1/;\n/plugin/;' sdt_out/pl.dtsi | dtc -@ -I dts -O dtb -o "${support_archive_name}.dtbo" -
awk -f tools/make_overlay.awk sdt_out/pl.dtsi | tee /tmp/pl.dts | dtc -@ -I dts -O dtb -o "${support_archive_name}.dtbo" -
unzip -o "${support_archive_name}.xsa" "${support_archive_name}.bit"

cat << EOF > /tmp/bitstream.bif
the_ROM_image:
{
    "${support_archive_name}.bit"
}
EOF

bootgen -image /tmp/bitstream.bif -arch zynq -process_bitstream bin -w on
