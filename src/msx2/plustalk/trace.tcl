set renderer none
set throttle off
after time 6.0 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 7.6 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 11 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 13 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 15 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 17 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 19 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 21 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 23 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 25 { keymatrixdown 8 0x01 ; after time 0.15 { keymatrixup 8 0x01 } }
after time 26.0 {
    set f [open "/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/msx2/plustalk/frame_26_0.vram" w]
    fconfigure $f -translation binary
    puts -nonewline $f [debug read_block {physical VRAM} 0 131072]
    close $f
    set f [open "/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/msx2/plustalk/frame_26_0.sat" w]
    fconfigure $f -translation binary
    puts -nonewline $f [debug read_block {physical VRAM} 64000 128]
    close $f
    set f [open "/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/msx2/plustalk/frame_26_0.ram" w]
    fconfigure $f -translation binary
    puts -nonewline $f [debug read_block memory 0 65536]
    close $f
    set f [open "/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/msx2/plustalk/frame_26_0.reg" w]
    fconfigure $f -translation binary
    puts -nonewline $f [debug read_block {VDP regs} 0 64]
    close $f
    set f [open "/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/msx2/plustalk/frame_26_0.pal" w]
    fconfigure $f -translation binary
    puts -nonewline $f [debug read_block {VDP palette} 0 32]
    close $f
}
after time [expr {[lindex [lsort -real [list 26.0]] end] + 0.25}] { exit 0 }