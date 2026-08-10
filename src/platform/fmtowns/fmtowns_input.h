#ifndef FMTOWNS_INPUT_H
#define FMTOWNS_INPUT_H

/* Raw 12-bit FM TOWNS pad status, PAD1 (port 0), read straight through to
 * src/platform/fmtowns/common/pad.c's fmt_pad_read(). Bit layout: see
 * pad.h -- 0=Up 1=Down 2=Left 3=Right 4=A 5=B 6=RUN 7=SELECT 8=Z 9=Y 10=X
 * 11=C, 0=pressed/1=released (idle-high wiring). Kept as this project's
 * own thin wrapper (rather than calling fmt_pad_read() directly from
 * fmtowns_main.c) so a later milestone can swap this body for a real
 * WaifuFmInput mapping without touching call sites. */
unsigned int fmtowns_input_read_pad1(void);

#endif /* FMTOWNS_INPUT_H */
