/*********************************************************************
 *                
 * Copyright (C) 1999-2005,  Karlsruhe University
 *                
 * File path:     afterburn-wedge/kaxen/linker.lds
 * Description:   KaXen linker script for x86.
 *                
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *                
 ********************************************************************/

ENTRY(kaxen_wedge_start)

#include INC_ARCH(page.h)

pgtab_region = CONFIG_WEDGE_VIRT;
xen_p2m_region = pgtab_region + CONFIG_WEDGE_PGTAB_REGION;
tmp_region = xen_p2m_region + CONFIG_WEDGE_P2M_REGION;
xen_shared_info = tmp_region + CONFIG_WEDGE_TMP_REGION;
_start_wedge = CONFIG_WEDGE_VIRT + CONFIG_WEDGE_WINDOW;

mapping_base_region = pgtab_region + (pgtab_region >> (PAGEDIR_BITS - PAGE_BITS));


SECTIONS
{
	.text _start_wedge :
	{
		*(.text)
		*(.gnu.linkonce.*)
	}

	.rodata . :
	{
		*(.rodata*)
	}
	
	. = ALIGN(4K);
	.data . :
	{
		*(.data)
		_bss_start = .;
		*(.bss)
		_bss_end = .;
	}

	_end_wedge = .;
}
