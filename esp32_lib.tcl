# esp32_lib.tcl - ESP32 ports helper library

proc q_rec {r b} {
    if {$r == 8} {
        puts $b
    } else {
        for {set c 0} {$c < 8} {incr c} {
            set k 1
            for {set i 0} {$i < $r} {incr i} {
                set y [lindex $b $i]
                if {$y == $c || [abs [expr $c - $y]] == [expr $r - $i]} {
                    set k 0
                    break
                }
            }
            if {$k} {
                set n $b
                lappend n $c
                q_rec [expr $r + 1] $n
            }
        }
    }
}

proc queens {} {
    q_rec 0 {}
}

proc help {} {
    puts "\n================================================="
    puts "      BareTcl ESP32 Help System (Version: 11)"
    puts "================================================="
    puts "Hardware Control Commands:"
    puts "  gpio_mode <pin> <mode>       : Set GPIO mode (0=INPUT, 1=OUTPUT, 2=INPUT_PULLUP)"
    puts "  digital_write <pin> <level>  : Set GPIO output level (0=LOW, 1=HIGH)"
    puts "  digital_read <pin>           : Read GPIO input level (returns 0 or 1)"
    puts ""
    puts "System Commands:"
    puts "  log <on|off>                 : Toggle Wi-Fi and system logs on/off"
    puts "  tcl_shell_ansi <0|1>         : Toggle ANSI terminal coloring on/off"
    puts "  ipconfig                     : Show Wi-Fi connection status and IP info"
    puts "  ping <host_or_ip>            : Send ICMP Echo requests to network host"
    puts "  sleep <ms>                   : Delay current task in milliseconds (non-blocking)"
    puts "  exit                         : Reboot the ESP32 chip"
    puts ""
    puts "Applications:"
    puts "  queens                       : Solve and output all 92 solutions of the 8-Queens problem"
    puts ""
    puts "All Registered Commands in Interpreter:"
    set cmds [__info_commands_core]
    puts "  $cmds"
    puts "================================================="
}
