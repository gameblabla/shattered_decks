/*
 * Keyboard/joystick-input hook for the supplied openMSX 21.0 headless binary.
 *
 * The original headless shell does not have a Keyboard object or a command
 * channel.  Its MSX PPI keyboard-read path is nevertheless present in
 * MSXMachine::inPort().  The patcher places this routine in the executable
 * padding of that binary and redirects the inPort entry to it.
 *
 * The wrapper appends this record to the temporary ROM passed to the backend:
 *
 *   frame_count * 11 active-low keyboard/joystick state bytes
 *     rows 0..8, joystick port 1, joystick port 2
 *   uint32 frame_count
 *   "KXMI"
 *
 * Rows are selected by the low nibble of the PPI C latch at 0x380BC.  The
 * current frame counter is at 0x38108 in the supplied backend's machine
 * object.  Unrecognised rows and missing records return 0xFF.
 */

.section .text
.global openmsx_input_hook
.type openmsx_input_hook, @function

openmsx_input_hook:
    movzbl  %sil, %eax
    cmpb    $0xA9, %al
    je      .keyboard
    cmpb    $0xA2, %al
    jne     .normal
    movzbl  0x380BA(%rdi), %ecx       /* selected joystick connector */
    shrl    $6, %ecx                  /* PSG R#15 bit 6: port 1/2 */
    andl    $1, %ecx
    addl    $9, %ecx
    jmp     .record

.keyboard:
    movzbl  0x380BC(%rdi), %ecx       /* selected keyboard row */
    andl    $0x0F, %ecx
    cmpl    $8, %ecx
    ja      .released

.record:
    mov     0x38(%rdi), %rdx
    cmpl    $0x494D584B, -4(%rdx)     /* trailer magic: KXMI */
    jne     .released
    mov     -8(%rdx), %r9d             /* frame count */
    testl   %r9d, %r9d
    jz      .released

    mov     0x38108(%rdi), %eax       /* emulated frame index */
    cmpl    %r9d, %eax
    jb      .frame_ok
    leal    -1(%r9), %eax
.frame_ok:
    imull   $11, %eax, %eax
    mov     %r9d, %r10d
    imull   $11, %r10d, %r10d
    addl    $8, %r10d
    sub     %r10, %rdx                 /* first row */
    add     %rax, %rdx
    mov     %ecx, %r10d
    add     %r10, %rdx
    movzbl  (%rdx), %eax
    ret

.released:
    movl    $0xFF, %eax
    ret

.normal:
    /* Recreate the original first instruction before its continuation. */
    movzbl  %sil, %eax
    /* The five-byte entry jump overwrote movzbl and cmp's opcode, but
     * leaves cmp's immediate byte at 0x12C5A8.  Continue after it. */
    jmp     0x12C5A9

.size openmsx_input_hook, .-openmsx_input_hook
