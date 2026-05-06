#!/usr/bin/awk -f

# This script converts the first device tree in the second one. It essentially
# peels the first two levels of the device tree and embeds the remaining content
# in a device tree overlay.

# / {
#         amba_pl: amba_pl {
#                 (...)
#         };
# };

# /dts-v1/;
# /plugin/;
#
# / {
#         fragment@0 {
#                 target-path = "/fpga-full";
#                 __overlay__ {
#                         (...)
#                 };
#         };
# };

function push_if_eq(n) {
    if (level == n) { level++ } else { print "bad" >> "/dev/stderr"; exit 1 }
}

function pop_if_lt(n) {
    if (level < n) { level-- } else { print "bad" >> "/dev/stderr"; exit 1 }
}

function check_eq(n) {
    if (level == n) { ; } else { print "bad" >> "/dev/stderr"; exit 1 }
}

BEGIN {
    print "/dts-v1/;\n/plugin/;\n"
    print "/ {"
    print "\tfragment@0 {"
    print "\t\ttarget-path = \"/fpga-full\";"
    print "\t\t__overlay__ {"

    level = 0
}

/^\/ \{/                     { push_if_eq(0); next }
/^[ \t]*amba_pl: amba_pl \{/ { push_if_eq(1); next }
/^[ \t]{0,1}\};$/            { pop_if_lt(3);  next }
/^[ \t]*$/                   { next }
                             { check_eq(2); print "\t" $0 }

END {
    check_eq(0);
    print "\t\t}; /* __overlay__ */"
    print "\t}; /* fragment@0 */"
    print "}; /* / */"
}
