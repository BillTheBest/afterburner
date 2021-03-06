/*********************************************************************
 *
 * Copyright (C) 2005,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/kaxen/cpu.h
 * Description:   
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
 * $Id: cpu.h,v 1.9 2005/12/16 12:12:45 joshua Exp $
 *
 ********************************************************************/
#ifndef __KAXEN__CPU_H__
#define __KAXEN__CPU_H__

#include <bitfield.h>
#include INC_ARCH(cpu.h)

struct xen_frame_id_t
{
    word_t id BITFIELD_32_64( 31, 63 );
    word_t error_code : 1;
};

// TODO amd64
struct xen_relink_frame_t {
    struct {
	word_t edx;
	word_t ecx;
	word_t eax;
    } clobbers;
    word_t fault_vaddr;
    word_t error_code;
    iret_user_frame_t iret;
};

struct xen_frame_t
{
#ifdef CONFIG_ARCH_AMD64
    word_t rax;
    word_t rcx;
    word_t rdx;
    word_t rbx;
    word_t rbp;
    word_t rsi;
    word_t rdi;
    word_t r8;
    word_t r9;
    word_t r10;
    word_t r11;
    word_t r12;
    word_t r13;
    word_t r14;
    word_t r15;

    struct
    {
	xen_frame_id_t frame_id;
	word_t fault_vaddr;
	word_t error_code;
    } info;

    iret_user_frame_t iret;

    word_t get_id()
	{ return info.frame_id.id; }
    bool uses_error_code()
	{ return info.frame_id.error_code; }
    bool is_page_fault()
	{ return get_id() == 14; }
    u32_t get_privilege()
	{ return cpu_t::get_segment_privilege(iret.cs); }
#elif defined(CONFIG_ARCH_IA32)
    word_t eax;
    word_t ecx;
    word_t edx;
    word_t ebx;
    word_t ebp;
    word_t esi;
    word_t edi;
    union {
	struct {
	    xen_frame_id_t frame_id;
	    word_t fault_vaddr;
	    word_t error_code;
	} info;
	struct {
	    word_t unused;
	    lret_frame_t lret;
	} trampoline;
	struct {
	    lret_frame_t lret;
	    word_t error_code;
	} err_trampoline;
    };
    iret_user_frame_t iret;

    word_t get_id()
	{ return info.frame_id.id; }
    bool uses_error_code()
	{ return info.frame_id.error_code; }
    bool is_page_fault()
	{ return get_id() == 14; }
    u32_t get_privilege()
	{ return cpu_t::get_segment_privilege(iret.cs); }
#else
#error "Not ported to this architecture!"
#endif

} __attribute__ ((packed));

extern void xen_deliver_async_vector( word_t vector, xen_frame_t *frame,
	bool use_error_code, bool prior_irq_flag=false );

#endif	/* __KAXEN__CPU_H__ */

