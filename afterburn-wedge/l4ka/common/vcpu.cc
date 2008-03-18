/*********************************************************************
 *
 * Copyright (C) 2005,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/l4-common/vm.cc
 * Description:   The L4 state machine for implementing the idle loop,
 *                and the concept of "returning to user".  When returning
 *                to user, enters a dispatch loop, for handling L4 messages.
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
#include <console.h>
#include <debug.h>
#include <bind.h>
#include <burn_symbols.h>

#include <l4/schedule.h>
#include <l4/ipc.h>

#include INC_WEDGE(vm.h)
#include INC_WEDGE(vcpu.h)
#include INC_WEDGE(monitor.h)
#include INC_WEDGE(l4privileged.h)
#include INC_WEDGE(backend.h)
#include INC_WEDGE(vcpulocal.h)
#include INC_WEDGE(hthread.h)
#include INC_WEDGE(message.h)
#include INC_WEDGE(irq.h)

typedef void (*vm_entry_t)();

static void vcpu_main_thread( void *param, hthread_t *hthread )
{
    backend_vcpu_init_t *init_info = 
    	(backend_vcpu_init_t *)hthread->get_tlocal_data();

    // Set our thread's local CPU.  This wipes out the hthread tlocal data.
    vcpu_t *vcpu_param =  (vcpu_t *) param;
    set_vcpu(*vcpu_param);
    vcpu_t &vcpu = get_vcpu();
    ASSERT(vcpu.cpu_id == vcpu_param->cpu_id);
    
    // Set our thread's exception handler.
    L4_Set_ExceptionHandler( vcpu.monitor_gtid );

    dprintf(debug_startup, "Entering main VM thread, TID %t\n", hthread->get_global_tid());

    vm_entry_t entry = (vm_entry_t) 0;
    
    dprintf(debug_startup, "%s main thread %t boot id %d\n", 
	    (init_info->vcpu_bsp ? "BSP" : "AP"),
	    hthread->get_global_tid() ,init_info->boot_id);

    if (init_info->vcpu_bsp)
    {   
	
	resourcemon_init_complete(); //MANUEL: was commented out
#if defined(CONFIG_WEDGE_STATIC)
	// Minor runtime binding to the guest OS.
	afterburn_exit_hook = backend_exit_hook;
	afterburn_set_pte_hook = backend_set_pte_hook;
#else
	// Load the kernel into memory and rewrite its instructions.
	if( !backend_load_vcpu(init_info) )
	    panic();
#endif
    
	// Prepare the emulated CPU and environment.  
	if( !backend_preboot(init_info) )
	    panic();

#if defined(CONFIG_VSMP)
	vcpu.turn_on();
#endif
	dprintf(debug_startup, "main thread executing guest OS IP %x SP %x\n", init_info->entry_ip, init_info->entry_sp);


	// Start executing the binary.
	__asm__ __volatile__ (
	    "movl %0, %%esp ;"
	    "push $0 ;" /* For FreeBSD. */
	    "push $0x1802 ;" /* Boot parameters for FreeBSD. */
	    "push $0 ;" /* For FreeBSD. */
	    "jmpl *%1 ;"
	    : /* outputs */
	    : /* inputs */
	      "a"(init_info->entry_sp), "b"(init_info->entry_ip),
	      "S"(init_info->entry_param)
	    );

	
    }
    else
    {
	// Virtual APs boot in protected mode w/o paging
	vcpu.set_kernel_vaddr(get_vcpu(0).get_kernel_vaddr());
	vcpu.cpu.enable_protected_mode();
	entry = (vm_entry_t) (init_info->entry_ip - vcpu.get_kernel_vaddr()); 
#if defined(CONFIG_VSMP)
	vcpu.turn_on();
#endif
	ASSERT(entry);    
	entry();
    }    
    
   
}
#if defined(CONFIG_L4KA_VMEXT)

/**
 * called on the migration destination host to reinitialize the VCPU
 * 1. set the new monitor TID
 * 2. create new IRQ thread and start operation
 * 3. create and resume all other VM threads
 */
bool vcpu_t::resume_vcpu()
{
    L4_Word_t priority;

    // set the monitor TID in the new VM
    monitor_gtid = L4_Myself();
    monitor_ltid = L4_MyLocalId();

    // Create and start the new IRQ thread.
    priority = get_vcpu_max_prio() + CONFIG_PRIO_DELTA_IRQ;
    irq_ltid = irq_init(priority, L4_Pager(), this);
    if( L4_IsNilThread(irq_ltid) )
    {
	printf( "Failed to initialize IRQ thread for VCPU %d\n", cpu_id);
	return false;
    }
    irq_gtid = L4_GlobalId( irq_ltid );
    //if (debug_startup)
    printf( "IRQ thread initialized tid %t VCPU %d\n", irq_gtid, cpu_id);
    
    // create and resume all other threads
    get_thread_manager().resume_vm_threads();
    
    return false;
}

#endif

bool vcpu_t::startup_vcpu(word_t startup_ip, word_t startup_sp, word_t boot_id, bool bsp)
{
    
    L4_Word_t preemption_control, priority;
    L4_ThreadId_t scheduler;
    L4_Error_t errcode;    
	
    // Setup the per-CPU VM stack.
    ASSERT(cpu_id < CONFIG_NR_VCPUS);    
    // I'm the monitor
    monitor_gtid = L4_Myself();
    monitor_ltid = L4_MyLocalId();


    ASSERT(startup_ip);
    if (!startup_sp)
	startup_sp = get_vcpu_stack();

#if defined(CONFIG_SMP)
    L4_Word_t num_pcpus = min((word_t) resourcemon_shared.pcpu_count, 
			      (word_t) L4_NumProcessors(L4_GetKernelInterface()));

    set_pcpu_id(cpu_id % num_pcpus);
    printf( "Set pcpu id to %d\n", cpu_id % num_pcpus, get_pcpu_id());
#endif
    
    // Create and start the IRQ thread.
    priority = get_vcpu_max_prio() + CONFIG_PRIO_DELTA_IRQ;
    irq_ltid = irq_init(priority, L4_Pager(), this);

    if( L4_IsNilThread(irq_ltid) )
    {
	printf( "Failed to initialize IRQ thread for VCPU %d\n", cpu_id);
	return false;
    }

    irq_gtid = L4_GlobalId( irq_ltid );
    dprintf(debug_startup, "IRQ thread initialized tid %t VCPU %d\n", irq_gtid, cpu_id);
    
    // Create the main VM thread.
    backend_vcpu_init_t init_info = 
	{ entry_sp	: startup_sp, 
	  entry_ip      : startup_ip, 
	  entry_param   : NULL, 
	  boot_id	: boot_id,
	  vcpu_bsp      : bsp,
	};
    
    priority = get_vcpu_max_prio() + CONFIG_PRIO_DELTA_MAIN;

    hthread_t *main_thread = get_hthread_manager()->create_thread(
	this,				// vcpu object
	get_vcpu_stack_bottom(),	// stack bottom
	get_vcpu_stack_size(),		// stack size
	priority,			// prio
	vcpu_main_thread,		// start func
	L4_Myself(),			// pager
	this,				// start param
	&init_info,			// thread local data
	sizeof(init_info)		// size of thread local data
	);

    if( !main_thread )
    {
	printf( "Failed to initialize main thread for VCPU %d\n", cpu_id);
	return false;
    }

    main_ltid = main_thread->get_local_tid();
    main_gtid = main_thread->get_global_tid();
    main_info.set_tid(main_gtid);
    main_info.mr_save.load_startup_reply((L4_Word_t) main_thread->start_ip, (L4_Word_t) main_thread->start_sp);
    
    preemption_control = (get_vcpu_max_prio() + CONFIG_PRIO_DELTA_IRQ << 16) | 2000;
    
#if defined(CONFIG_L4KA_VMEXT)
    scheduler = monitor_gtid;
#else
    scheduler = main_gtid;
#endif
    
    errcode = ThreadControl( main_gtid, main_gtid, scheduler, L4_nilthread, (word_t) -1 );
    if (errcode != L4_ErrOk)
    {
	printf( "Error: unable to set main thread's scheduler %t L4 error: %s\n",
		scheduler, L4_ErrString(errcode));
	return false;
    }

#if defined(CONFIG_L4KA_VMEXT)
    scheduler = monitor_gtid;
    
    /* Set exception ctrlxfer mask */
    L4_Msg_t ctrlxfer_msg;
    L4_Word64_t fault_id_mask = (1<<2) | (1<<3) | (1<<5);
    L4_Word_t fault_mask = L4_CTRLXFER_FAULT_MASK(L4_CTRLXFER_GPREGS_ID);
    L4_Clear(&ctrlxfer_msg);
    L4_AppendFaultConfCtrlXferItems(&ctrlxfer_msg, fault_id_mask, fault_mask);
    L4_Load(&ctrlxfer_msg);
    L4_ConfCtrlXferItems(main_gtid);
#else
    scheduler = main_gtid;
#endif
    

    
#if defined(CONFIG_VSMP)
    bool mbt = remove_vcpu_hthread(main_gtid);
    ASSERT(mbt);
    hthread_info.init();
#endif
    
    dprintf(debug_startup, "Main thread initialized tid %t VCPU %d\n", main_gtid, cpu_id);
    return true;

}   



#if defined(CONFIG_VSMP)
extern "C" void NORETURN vcpu_monitor_thread(vcpu_t *vcpu_param, word_t boot_vcpu_id, 
	word_t startup_ip, word_t startup_sp)
{

    set_vcpu(*vcpu_param);
    vcpu_t &vcpu = get_vcpu();
    
    ASSERT(vcpu.cpu_id == vcpu_param->cpu_id);
    ASSERT(boot_vcpu_id < CONFIG_NR_VCPUS);
    ASSERT(get_vcpu(boot_vcpu_id).cpu_id == boot_vcpu_id);
 
    // Change Pager
    L4_Set_Pager (resourcemon_shared.resourcemon_tid);
    
#if !defined(CONFIG_SMP_ONE_AS)
    // Initialize VCPU local data
    vcpu.init_local_mappings(vcpu.cpu_id);
#endif
    
    dprintf(debug_startup, "monitor thread's TID: %t boot VCPU %d startup IP %x SP %x\n",
	    L4_Myself(), boot_vcpu_id, startup_ip, startup_sp);

    // Flush complete address space, to get it remapped by resourcemon
    //L4_Flush( L4_CompleteAddressSpace + L4_FullyAccessible );
    vcpu.set_pcpu_id(0);
    
    // Startup AP VM
    vcpu.startup_vcpu(startup_ip, startup_sp, boot_vcpu_id, false);

    // Enter the monitor loop.
    get_vcpu(boot_vcpu_id).bootstrap_other_vcpu_done();

    monitor_loop(vcpu, get_vcpu(boot_vcpu_id) );
    
    printf( "PANIC, monitor fell through\n");       
    panic();
}

bool vcpu_t::startup(word_t vm_startup_ip)
{
    vcpu_t &boot_vcpu = get_vcpu();
    
    L4_Error_t errcode;
    // Create a monitor task
    monitor_gtid =  get_hthread_manager()->thread_id_allocate();
    if( L4_IsNilThread(monitor_gtid) )
	PANIC( "Failed to allocate monitor thread for VCPU %d : out of thread IDs.", 
		boot_vcpu.cpu_id );

    // Set monitor priority.
    L4_Word_t monitor_prio = get_vcpu_max_prio() + CONFIG_PRIO_DELTA_MONITOR;

    L4_Word_t utcb_area = afterburn_utcb_area;
    L4_ThreadId_t vcpu_space = monitor_gtid;

#if defined(CONFIG_SMP_ONE_AS)
    vcpu_space = L4_Myself();
#endif

	    
    // Create the monitor thread.
    errcode = ThreadControl( 
	monitor_gtid,		   // monitor
	vcpu_space,		   // space
	boot_vcpu.main_gtid,	   // scheduler
	L4_nilthread,		   // pager
	monitor_prio,              // priority
	utcb_area		   // utcb_location
	);
	
    if( errcode != L4_ErrOk )
	PANIC( "Failed to create monitor thread for VCPU %d TID %t L4 error %s\n",
		boot_vcpu.cpu_id, monitor_gtid, L4_ErrString(errcode));

    // Create the monitor address space
#if defined(CONFIG_SMP_ONE_AS)
    L4_Fpage_t utcb_fp = L4_Fpage( (L4_Word_t) afterburn_utcb_area + 4096 * cpu_id, CONFIG_UTCB_AREA_SIZE );
#else
    L4_Fpage_t utcb_fp = L4_Fpage( (L4_Word_t) afterburn_utcb_area, CONFIG_UTCB_AREA_SIZE );
#endif
    L4_Fpage_t kip_fp = L4_Fpage( (L4_Word_t) afterburn_kip_area, CONFIG_KIP_AREA_SIZE);

    errcode = SpaceControl( monitor_gtid, 0, kip_fp, utcb_fp, L4_nilthread );
	
    if( errcode != L4_ErrOk )
	PANIC( "Failed to create monitor address space for VCPU %d TID %t L4 error %s\n",
		boot_vcpu.cpu_id, monitor_gtid, L4_ErrString(errcode));
	
   
    // Make the monitor thread valid.
    errcode = ThreadControl( 
	monitor_gtid,			// monitor
	monitor_gtid,			// new aS
	boot_vcpu.monitor_gtid,		// scheduler, for startup
#if defined(CONFIG_SMP_ONE_AS)
	resourcemon_shared.cpu[get_pcpu_id()].resourcemon_tid,
	afterburn_utcb_area + 4096 * cpu_id,
#else
	boot_vcpu.monitor_gtid,	// pager, for activation msg
	afterburn_utcb_area,
#endif
	monitor_prio
	);
    
    
    if( errcode != L4_ErrOk )
	PANIC( "Failed to make valid monitor address space for VCPU %d TID %t L4 error %s\n",
		boot_vcpu.cpu_id, monitor_gtid, L4_ErrString(errcode));

    
#if defined(CONFIG_L4KA_VMEXT)
    L4_Msg_t ctrlxfer_msg;
    L4_Word64_t fault_id_mask = (1<<2) | (1<<3) | (1<<5);
    L4_Word_t fault_mask = L4_CTRLXFER_FAULT_MASK(L4_CTRLXFER_GPREGS_ID);
    L4_Clear(&ctrlxfer_msg);
    L4_AppendFaultConfCtrlXferItems(&ctrlxfer_msg, fault_id_mask, fault_mask);
    L4_Load(&ctrlxfer_msg);
    L4_ConfCtrlXferItems(monitor_gtid);
#endif
    
  
    word_t *vcpu_monitor_params = (word_t *) (afterburn_monitor_stack[cpu_id] + KB(16));

    vcpu_monitor_params[0]  = get_vcpu_stack();
    vcpu_monitor_params[-1] = vm_startup_ip;
    vcpu_monitor_params[-2] = boot_vcpu.cpu_id;
    vcpu_monitor_params[-3] = (word_t) this;
    
    word_t vcpu_monitor_sp = (word_t) vcpu_monitor_params;
    
    // Ensure that the monitor stack conforms to the function calling ABI.
    vcpu_monitor_sp = (vcpu_monitor_sp - CONFIG_STACK_SAFETY) & ~(CONFIG_STACK_ALIGN-1);
    
    dprintf(debug_startup, "starting up monitor %t VCPU %d IP %x SP  %x\n",
	    monitor_gtid, cpu_id, vcpu_monitor_thread, vcpu_monitor_sp);
    
   
    boot_vcpu.bootstrap_other_vcpu(cpu_id);	    
    monitor_info.mr_save.load_startup_reply(
	(L4_Word_t) vcpu_monitor_thread, (L4_Word_t) vcpu_monitor_sp);
    monitor_info.mr_save.set_propagated_reply(boot_vcpu.monitor_gtid); 	
    monitor_info.mr_save.load();
    boot_vcpu.main_info.mr_save.load_yield_msg(monitor_gtid);

    L4_MsgTag_t tag = L4_Send(monitor_gtid);


    while (is_off())
	L4_ThreadSwitch(monitor_gtid);

    if (!L4_IpcSucceeded(tag))
	PANIC( "Failed to activate monitor for VCPU %d TID %t L4 error %s\n",
		boot_vcpu.cpu_id, monitor_gtid, L4_ErrString(errcode));

    dprintf(debug_startup, "AP startup sequence for VCPU %d done\n.", cpu_id);
    
    return true;
}   

#endif  /* defined(CONFIG_VSMP) */ 