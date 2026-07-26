/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 * Modified for ESP32-S31 Flash XIP + uncached PSRAM data.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

	ASSERT(FW_TEXT_START == 0x40030000,
	       "ESP32-S31 OpenSBI XIP must be linked at 0x40030000")
	ASSERT(FW_RW_START >= 0xC0F00000 && FW_RW_START < 0xC1000000,
	       "ESP32-S31 OpenSBI RW must use the reserved uncached PSRAM region")

	. = FW_TEXT_START;
	/* Don't add any section between FW_TEXT_START and _fw_start */
	PROVIDE(_fw_start = .);

	. = ALIGN(0x1000); /* Need this to create proper sections */

	/* Beginning of the code section */

	.text :
 	{
		PROVIDE(_text_start = .);
		*(.entry)
		*(.text)
		*(.text.*)
		. = ALIGN(8);
		PROVIDE(_text_end = .);
	}

	/* End of the code sections */

	. = ALIGN(0x1000); /* Ensure next section is page aligned */

	/* Beginning of the read-only data sections */

	PROVIDE(_rodata_start = .);

	.rodata :
	{
		*(.rodata .rodata.*)
		. = ALIGN(8);
	}

	.dynsym :
	{
		*(.dynsym)
	}

	. = ALIGN(0x1000); /* Ensure next section is page aligned */

	.rela.dyn : {
		PROVIDE(__rela_dyn_start = .);
		*(.rela*)
		PROVIDE(__rela_dyn_end = .);
	}

	PROVIDE(_rodata_end = .);

	/* End of the read-only data sections */

	/*
	 * Flash-resident data (LMA) will be copied to uncached PSRAM (VMA).
	 * LMA is placed immediately after .rela.dyn so the binary
	 * layout stays contiguous (objcopy won't insert a gap).
	 */
	PROVIDE(__data_flash_start = ADDR(.rela.dyn) + SIZEOF(.rela.dyn));

	. = FW_RW_START;                  /* switch to uncached PSRAM VMA */

	PROVIDE(_fw_rw_start = .);

	/* Beginning of the read-write data sections */

	.data : AT(__data_flash_start)
	{
		PROVIDE(_data_start = .);

		*(.sdata)
		*(.sdata.*)
		*(.data)
		*(.data.*)
		*(.readmostly.data)
		*(*.data)
		. = ALIGN(8);

		PROVIDE(_data_end = .);
	}

	. = ALIGN(0x1000); /* Ensure next section is page aligned */

	.bss (NOLOAD) :
	{
		PROVIDE(_bss_start = .);
		*(.sbss)
		*(.sbss.*)
		*(.bss)
		*(.bss.*)
		. = ALIGN(8);
		PROVIDE(_bss_end = .);
	}

	/* End of the read-write data sections */

	. = ALIGN(0x1000); /* Need this to create proper sections */

	PROVIDE(_fw_end = .);
