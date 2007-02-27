/*********************************************************************
 *
 * Copyright (C) 2005,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/include/l4ka/vcpu.h
 * Description:   The per-CPU data declarations for the L4Ka environment.
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
 * $Id: vcpu.h,v 1.21 2006/09/21 13:49:27 joshua Exp $
 *
 ********************************************************************/
#ifndef __AFTERBURN_WEDGE__INCLUDE__L4KA__VCPU_H__
#define __AFTERBURN_WEDGE__INCLUDE__L4KA__VCPU_H__

#define OFS_VCPU_CPU	   32
#define OFS_CPU_FLAGS	   (0 + OFS_VCPU_CPU)
#define OFS_CPU_CS	   (4 + OFS_VCPU_CPU)
#define OFS_CPU_SS	   (8 + OFS_VCPU_CPU)
#define OFS_CPU_TSS	   (12 + OFS_VCPU_CPU)
#define OFS_CPU_IDTR	   (18 + OFS_VCPU_CPU)
#define OFS_CPU_CR2	   (40 + OFS_VCPU_CPU)
#define OFS_CPU_REDIRECT   (64 + OFS_VCPU_CPU)


#if defined(ASSEMBLY)
#define vcpu	__vcpu
#else


#include <l4/thread.h>
#include INC_ARCH(cpu.h)
#include INC_WEDGE(user.h)
#include INC_WEDGE(debug.h)
#include INC_WEDGE(resourcemon.h)

struct map_info_t
{
    word_t addr;
    word_t page_bits;
    word_t rwx;
};

struct vcpu_t
{
    L4_ThreadId_t monitor_ltid;		// 0
    L4_ThreadId_t monitor_gtid;		// 4
    L4_ThreadId_t irq_ltid;		// 8
    L4_ThreadId_t irq_gtid;		// 12
    L4_ThreadId_t main_ltid;		// 16
    L4_ThreadId_t main_gtid;		// 20
    word_t vcpu_stack;			// 24
    word_t vcpu_stack_bottom;		// 28

    cpu_t cpu;				// 32
    word_t cpu_id;
    word_t cpu_hz;
    
    volatile bool dispatch_ipc;			
    private:
    burn_redirect_frame_t *idle_frame;	
    public:
    L4_Word8_t		magic[8];	   

    static const word_t vcpu_stack_size = KB(32);
    static word_t nr_vcpus, nr_pcpus;

    word_t guest_vaddr_offset;

    word_t vaddr_flush_min;
    word_t vaddr_flush_max;
    bool   vaddr_global_only;
    bool   global_is_crap;
    word_t wedge_vaddr_end;

    thread_info_t main_info;
    thread_info_t irq_info;
    thread_info_t monitor_info;
    thread_info_t *user_info;

    word_t  pcpu_id;

    void vaddr_stats_reset()
	{
	    vaddr_flush_max = 0; 
	    vaddr_flush_min = ~vaddr_flush_max;
	    vaddr_global_only = true;
	    global_is_crap = false;
	}

    void vaddr_stats_update( word_t vaddr, bool is_global)
	{
	    if(vaddr > vaddr_flush_max) vaddr_flush_max = vaddr;
	    if(vaddr < vaddr_flush_min) vaddr_flush_min = vaddr;
	    if( !is_global)
		vaddr_global_only = false;
	    else if( !cpu.cr4.is_page_global_enabled() )
		global_is_crap = true;
	}
    bool is_global_crap()
	{ return global_is_crap; }
    void invalidate_globals()
	{ global_is_crap = true; }
    
    bool handle_wedge_pfault(thread_info_t *ti, map_info_t &map_info, bool &nilmapping);
    bool resolve_paddr(thread_info_t *ti, map_info_t &map_info, word_t &paddr, bool &nilmapping);
    
    word_t get_id()
	{ return cpu_id; }

    volatile bool in_dispatch_ipc()
	{ return dispatch_ipc; }
    void dispatch_ipc_enter()
	{ ASSERT(dispatch_ipc == false); dispatch_ipc = true; }
    void dispatch_ipc_exit()
	{ ASSERT(dispatch_ipc == true); dispatch_ipc = false; }

    volatile bool is_idle()
	{ return (idle_frame != NULL); }
    void idle_enter(burn_redirect_frame_t *frame)
	{ ASSERT(!is_idle()); idle_frame = frame; }
    void idle_exit() 
	{ ASSERT(is_idle()); idle_frame = NULL; }
    bool redirect_idle()
	{ 
	    if (is_idle() && idle_frame->do_redirect())
	    {
		idle_exit();
		return true;
	    }
	    return false;
	}
#if defined(CONFIG_VSMP)
    static const word_t max_vcpu_hthreads = 16;
    L4_ThreadId_t vcpu_hthreads[max_vcpu_hthreads];
    thread_info_t hthread_info;
    enum startup_status_e {status_off=0, status_bootstrap=1, status_on=2};
    volatile word_t  startup_status;
    word_t  booted_cpu_id;
    
    void turn_on()
	{ ASSERT(startup_status != status_on); startup_status = status_on; }
    bool is_off()
	{ return startup_status == status_off; }
    bool is_booting_other_vcpu()
	{ return startup_status == status_bootstrap; }
    word_t get_booted_cpu_id()
	{ ASSERT(startup_status == status_bootstrap); return booted_cpu_id; }
    void bootstrap_other_vcpu(word_t dest_id)
	{ 
	    ASSERT(startup_status != status_bootstrap); 
	    startup_status = status_bootstrap; 
	    booted_cpu_id = dest_id;
	}
    void bootstrap_other_vcpu_done()
	{ 
	    ASSERT(startup_status == status_bootstrap); 
	    startup_status = status_on; 
	}
    
    bool add_vcpu_hthread(L4_ThreadId_t htid)
	{
	    ASSERT(htid != L4_nilthread);
	    for (word_t i=0; i < max_vcpu_hthreads; i++)
		if (vcpu_hthreads[i] == L4_nilthread)
		{
		    vcpu_hthreads[i] = htid;
		    return true;
		}
	    return false;
	}
    bool remove_vcpu_hthread(L4_ThreadId_t htid)
	{
	    ASSERT(htid != L4_nilthread);
	    for (word_t i=0; i < max_vcpu_hthreads; i++)
		if (vcpu_hthreads[i] == htid)
		{
		    vcpu_hthreads[i] = L4_nilthread;
		    return true;
		}
	    return false;	
	}

    bool is_vcpu_hthread(L4_ThreadId_t htid)
	{
	    ASSERT(htid != L4_nilthread);
	    for (word_t i=0; i < max_vcpu_hthreads; i++)
		if (vcpu_hthreads[i] == htid)
		    return true;
	    return false;
	}

#endif
    
    bool is_vcpu_ktid(L4_ThreadId_t gtid)
	{
	    return (gtid == monitor_gtid || gtid == main_gtid);
	}
    
    word_t get_vcpu_stack()
	{ return vcpu_stack; }
    word_t get_vcpu_stack_bottom()
	{ return vcpu_stack_bottom; }
    word_t get_vcpu_stack_size()
	{ return vcpu_stack_size; }

    void set_kernel_vaddr( word_t vaddr )
	{
	    if( vaddr > get_wedge_vaddr() )
		PANIC( "Kernel link address is too high\n");
	    guest_vaddr_offset = vaddr;
	}
    word_t get_kernel_vaddr()
	{ return guest_vaddr_offset; }

#if !defined(CONFIG_WEDGE_STATIC)
    word_t get_wedge_vaddr()
	{ return CONFIG_WEDGE_VIRT; }
    word_t get_wedge_paddr()
	{ return CONFIG_WEDGE_PHYS; }
    word_t get_wedge_end_paddr()
	{
	    extern word_t _end_wedge[];
	    word_t end_vaddr = (((word_t)_end_wedge - 1) + PAGE_SIZE) & PAGE_MASK;
	    return end_vaddr - get_wedge_vaddr() + get_wedge_paddr();
	}
    word_t get_wedge_end_static()
	{ return wedge_vaddr_end - (CONFIG_WEDGE_VIRT_BUBBLE_PAGES * PAGE_SIZE); }
    word_t get_wedge_end_vaddr()
	{ return wedge_vaddr_end; }
#endif
    

    word_t get_vcpu_max_prio()
	{ return resourcemon_shared.prio; }


    bool is_valid_vcpu() 
	{ return (magic[0] == 'V' && magic[1] == 'C' &&
		  magic[2] == 'P' && magic[3] == 'U' &&
		  magic[4] == 'V' && magic[5] == '0');
	}    
    
    void init(word_t id, word_t hz);
    void init_local_mappings(void);
    
    bool startup(word_t vm_startup_ip);
    bool startup_vcpu(word_t startup_ip, word_t startup_sp, word_t boot_id, bool bsp);
    
};

#endif	/* !ASSEMBLY */

#endif	/* __AFTERBURN_WEDGE__INCLUDE__L4KA__VCPU_H__ */
