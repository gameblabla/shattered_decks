        .text

        .align  4
        .global sh2_app_start
sh2_app_start:
        .incbin "build/cd32x/sh2/waifucd32x.sh2.bin"

        .align  4
sh2_app_end:

        .global sh2_app_length
sh2_app_length:
        .long   sh2_app_end - sh2_app_start
