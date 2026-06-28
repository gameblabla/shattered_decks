    .text
    .align 2

/*
 * Thin PC-FX BIOS filesystem dispatcher thunks.
 *
 * The PC-FX and PC-FXGA BIOS both expose the public filesystem dispatcher at
 * 0xFFF5A000, but their internal function tables differ.  Call the dispatcher
 * by index instead of jumping to retail PC-FX-only private routine addresses.
 *
 * Homebrew code and the BIOS use different gp values.  These thunks save the
 * caller context required by GCC, set the BIOS gp, call the dispatcher, then
 * restore the homebrew context.  The BIOS return value is r10.
 */

    .macro BIOSFS_PROLOGUE
    addi -48, sp, sp
    st.w lp, 0[sp]
    st.w gp, 4[sp]
    st.w r20, 8[sp]
    st.w r21, 12[sp]
    st.w r22, 16[sp]
    st.w r23, 20[sp]
    st.w r24, 24[sp]
    st.w r25, 28[sp]
    st.w r26, 32[sp]
    st.w r27, 36[sp]
    st.w r28, 40[sp]
    st.w r29, 44[sp]
    .endm

    .macro BIOSFS_EPILOGUE
    ld.w 44[sp], r29
    ld.w 40[sp], r28
    ld.w 36[sp], r27
    ld.w 32[sp], r26
    ld.w 28[sp], r25
    ld.w 24[sp], r24
    ld.w 20[sp], r23
    ld.w 16[sp], r22
    ld.w 12[sp], r21
    ld.w 8[sp], r20
    ld.w 4[sp], gp
    ld.w 0[sp], lp
    addi 48, sp, sp
    jmp lp
    .endm

    .global _pcfx_biosfs_call0
_pcfx_biosfs_call0:
    BIOSFS_PROLOGUE
    mov r6, r10
    movhi 32, r0, gp
    movea -32768, gp, gp
    jal 0xfff5a000
    BIOSFS_EPILOGUE

    .global _pcfx_biosfs_call1
_pcfx_biosfs_call1:
    BIOSFS_PROLOGUE
    mov r6, r10
    mov r7, r6
    movhi 32, r0, gp
    movea -32768, gp, gp
    jal 0xfff5a000
    BIOSFS_EPILOGUE

    .global _pcfx_biosfs_call2
_pcfx_biosfs_call2:
    BIOSFS_PROLOGUE
    mov r6, r10
    mov r7, r6
    mov r8, r7
    movhi 32, r0, gp
    movea -32768, gp, gp
    jal 0xfff5a000
    BIOSFS_EPILOGUE

    .global _pcfx_biosfs_call3
_pcfx_biosfs_call3:
    BIOSFS_PROLOGUE
    mov r6, r10
    mov r7, r6
    mov r8, r7
    mov r9, r8
    movhi 32, r0, gp
    movea -32768, gp, gp
    jal 0xfff5a000
    BIOSFS_EPILOGUE
