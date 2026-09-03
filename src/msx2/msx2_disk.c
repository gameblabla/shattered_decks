// ─────────────────────────────────────────────────────────────────────────────
//  msx2_disk.c — the continue code, on a floppy
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_disk.h"

#define DISK_SECTOR_BYTES  512
#define DISK_CODE_LEN      16
#define DISK_MAGIC_LEN     8

// The boot record's own fields, at their standard offsets.
#define BOOT_TOTAL_SECTORS 0x13   // word
#define BOOT_MEDIA_ID      0x15

static const c8 g_magic[DISK_MAGIC_LEN] = { 'S','H','A','T','D','E','C','K' };

static u8  g_buf[DISK_SECTOR_BYTES];
static u8  g_probed;
static u8  g_slot;                 // the disk ROM's slot id, 0xFF for none

// The inter-slot call's arguments.  They travel through globals because the
// call itself is inline assembly and SDCC's frame pointer is not something to
// rely on inside a routine that swaps page 1 out from under the caller.
static u16 g_iy;                   // slot id in the high byte, for LD IY,(nn)
static u16 g_sector;
static u8* g_addr;
static u8  g_count;
static u8  g_media;
static u8  g_write;
static u8  g_err;

// DSKIO, at 0x4010 of the disk ROM.
//   A  = drive (0 is the first)          B  = sectors
//   C  = media descriptor                DE = first logical sector
//   HL = transfer address (in RAM: page 1 is the disk ROM during the call)
//   carry set = write, clear = read;  carry set on return = failure.
// PUTTING THE BIOS BACK IN PAGE 0.
//
// This whole module talks to the BIOS -- RDSLT to look at another slot, CALSLT
// to enter the disk ROM -- and both of those entries are at 0x000C and 0x001C,
// which on this cartridge is not the BIOS at all: crt0 puts the game's own
// slot in page 0 (INIT_P1_TO_P02) and msx2_bank.c then swaps code banks
// through it.  Calling either one lands in the middle of the duel screen.
//
// So the primary slot for page 0 alone is set back to the one the BIOS booted
// from, for the length of the call, and restored afterwards.  Three things
// make that enough:
//
//  * only page 0 moves.  This module is compiled into _CODE at 0x4000, and
//    CALSLT is designed to be called from there: it switches page 1 to the
//    disk ROM itself and puts it back before it returns;
//  * the subslot half of the address never has to be touched.  Slot 0's own
//    subslot register still holds whatever the BIOS latched for page 0 at
//    boot -- nothing here has written 0xFFFF -- so restoring the primary is
//    the whole job, and the 0xFFFF dance that would need slot 0 selected in
//    page 3 (where the stack lives) is avoided entirely;
//  * interrupts are off throughout.  The handler is in RAM page 3 (IM 2, see
//    project_config.js) and would survive, but the disk ROM is not somewhere
//    to take one.
//
// This is also why the disk layer is the one screen-adjacent thing NOT in a
// page-0 code bank: a bank that has to disappear for the call to work cannot
// be the bank the call is made from.
static u8 g_slot_save;

static void Msx2_DiskBiosIn(void)
{
	__asm
		di
		in		a, (#0xA8)
		ld		(_g_slot_save), a
		and		a, #0xFC
		ld		hl, #_g_EXPTBL
		or		a, (hl)             // ... the primary the BIOS booted from
		and		a, #0x03
		ld		b, a
		ld		a, (_g_slot_save)
		and		a, #0xFC
		or		a, b
		out		(#0xA8), a
	__endasm;
}

static void Msx2_DiskBiosOut(void)
{
	__asm
		ld		a, (_g_slot_save)
		out		(#0xA8), a
		ei
	__endasm;
}

static void Msx2_DiskIo(void)
{
	__asm
		push	ix
		push	iy
		ld		iy, (_g_iy)
		ld		ix, #0x4010
		ld		hl, (_g_addr)
		ld		de, (_g_sector)
		ld		a, (_g_count)
		ld		b, a
		ld		a, (_g_media)
		ld		c, a
		ld		a, (_g_write)
		or		a                   // Z when this is a read
		ld		a, #0               // drive A; LD does not touch the flags
		scf                         // assume a write
		jr		nz, 00002$
		ccf                         // ... and take the carry back off for a read
	00002$:
		di
		call	#0x001C             // CALSLT: the disk ROM maps itself in
		ld		a, #0
		jr		nc, 00003$
		ld		a, #1
	00003$:
		ld		(_g_err), a
		ei
		pop		iy
		pop		ix
	__endasm;
}

// Is `slot` holding a disk ROM?  A ROM in page 1 starts with "AB", and a disk
// ROM follows it with the four-entry jump table every disk interface has:
// DSKIO, DSKCHG, GETDPB and CHOICE, three bytes apart.  A cartridge that is
// merely a ROM -- this game, for one -- has its own data there instead.
static bool Msx2_DiskIsRom(u8 slot)
{
	bool is_disk;

	Msx2_DiskBiosIn();
	is_disk = (BIOS_InterSlotRead(slot, 0x4000) == 'A')
	       && (BIOS_InterSlotRead(slot, 0x4001) == 'B')
	       && (BIOS_InterSlotRead(slot, 0x4010) == 0xC3)
	       && (BIOS_InterSlotRead(slot, 0x4013) == 0xC3)
	       && (BIOS_InterSlotRead(slot, 0x4016) == 0xC3)
	       && (BIOS_InterSlotRead(slot, 0x4019) == 0xC3);
	Msx2_DiskBiosOut();
	return is_disk;
}

static void Msx2_DiskFind(void)
{
	u8 prim;

	// The jump table below is the whole test.  A BDOS vector at 0xF37D would
	// have been a cheaper first gate, but only Disk BASIC installs one, and an
	// interface without it still has a disk ROM this can talk to.
	g_probed = TRUE;
	g_slot = 0xFF;

	for(prim = 0; prim < 4; ++prim)
	{
		u8 exp = g_EXPTBL[prim];
		if(exp & 0x80)
		{
			u8 sub;
			for(sub = 0; sub < 4; ++sub)
			{
				u8 id = (u8)(0x80 | (sub << 2) | prim);
				if(Msx2_DiskIsRom(id))
				{
					g_slot = id;
					return;
				}
			}
		}
		else if(Msx2_DiskIsRom(prim))
		{
			g_slot = prim;
			return;
		}
	}
}

bool Msx2_DiskPresent(void)
{
	if(!g_probed)
		Msx2_DiskFind();
	return (g_slot != 0xFF);
}

// One sector, in or out.  Everything goes through the boot record first: it
// says how many sectors the disk has and what its media descriptor is, so the
// save lands on the real last sector of whatever is in the drive rather than on
// a guessed one.
static bool Msx2_DiskSector(u16* sector, bool write)
{
	if(!Msx2_DiskPresent())
		return FALSE;

	g_iy = (u16)((u16)g_slot << 8);
	g_addr = g_buf;
	g_count = 1;

	if(*sector == 0)
	{
		g_media = 0xF9;              // 720 KB, the usual answer and a safe ask
		g_sector = 0;
		g_write = FALSE;
		Msx2_DiskBiosIn();
		Msx2_DiskIo();
		Msx2_DiskBiosOut();
		if(g_err)
			return FALSE;
		{
			u16 total = (u16)(g_buf[BOOT_TOTAL_SECTORS] |
			                  ((u16)g_buf[BOOT_TOTAL_SECTORS + 1] << 8));
			if(total < 16)
				return FALSE;        // not a formatted disk
			g_media = g_buf[BOOT_MEDIA_ID];
			*sector = (u16)(total - 1);
		}
		return TRUE;
	}

	g_sector = *sector;
	g_write = write ? TRUE : FALSE;
	Msx2_DiskBiosIn();
	Msx2_DiskIo();
	Msx2_DiskBiosOut();
	return (g_err == 0);
}

bool Msx2_DiskSave(const c8* code)
{
	u16 sector = 0;
	u16 j;
	u8 i;

	if(!Msx2_DiskSector(&sector, FALSE))
		return FALSE;
	// u16, deliberately: the sector is 512 bytes and a u8 counter never
	// reaches it.
	for(j = 0; j < DISK_SECTOR_BYTES; ++j)
		g_buf[j] = 0;
	for(i = 0; i < DISK_MAGIC_LEN; ++i)
		g_buf[i] = (u8)g_magic[i];
	for(i = 0; i < DISK_CODE_LEN; ++i)
		g_buf[DISK_MAGIC_LEN + i] = (u8)code[i];
	return Msx2_DiskSector(&sector, TRUE);
}

bool Msx2_DiskLoad(c8* code)
{
	u16 sector = 0;
	u8 i;

	if(!Msx2_DiskSector(&sector, FALSE))
		return FALSE;
	if(!Msx2_DiskSector(&sector, FALSE))
		return FALSE;
	for(i = 0; i < DISK_MAGIC_LEN; ++i)
		if(g_buf[i] != (u8)g_magic[i])
			return FALSE;
	for(i = 0; i < DISK_CODE_LEN; ++i)
		code[i] = (c8)g_buf[DISK_MAGIC_LEN + i];
	code[DISK_CODE_LEN] = 0;
	return TRUE;
}
