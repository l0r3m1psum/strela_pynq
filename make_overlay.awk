#!/usr/bin/awk -f

BEGIN {
    print "/dts-v1/;"
    print "/plugin/;"
    print ""
}

/^\/ {/ {
    print "/ {"
    print "\tfragment@0 {"
    print "\t\ttarget-path = \"/fpga-full\";"
    print "\t\t__overlay__ {"
    depth = 1
    root_found = 1
    next
}

root_found {
    temp = $0

    num_open_braces = gsub(/\{/, "", temp)
    num_close_braces = gsub(/\}/, "", temp)

    depth += (num_open_braces - num_close_braces)

    if (depth == 0) {
        print "\t\t}; /* close __overlay__ */"
        print "\t}; /* close fragment@0 */"
        print "}; /* close root */"
        root_found = 0
        next
    }

    print "\t\t" $0
}

!root_found {
    if ($0 != "") print $0
}
