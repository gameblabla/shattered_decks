/* 386SX fixed-point primitive used by the grouped Layer-II quantizers.
 * GAS AT&T syntax: one-operand imulw multiplies AX by the 16-bit source
 * and returns the exact signed 32-bit product in DX:AX. */
        .text
        .globl fmt_mp2_mul16
        .type fmt_mp2_mul16,@function
fmt_mp2_mul16:
        movw    4(%esp),%ax
        imulw   8(%esp)
        movzwl  %ax,%eax
        shll    $16,%edx
        orl     %edx,%eax
        ret
        .size fmt_mp2_mul16,.-fmt_mp2_mul16
