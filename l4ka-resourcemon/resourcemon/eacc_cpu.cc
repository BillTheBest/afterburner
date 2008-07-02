/*********************************************************************
 *                
 * Copyright (C) 2006-2008,  Karlsruhe University
 *                
 * File path:     eacc_cpu.cc
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#include <l4/types.h>
#include <l4/kip.h>
#include <common/hthread.h>
#include <resourcemon/resourcemon.h>
#include <resourcemon/vm.h>
#include <resourcemon/earm.h>

static L4_Word64_t  last_acc_timestamp[L4_LOGGING_MAX_CPUS][L4_LOGGING_MAX_DOMAINS];

IEarm_shared_t *manager_cpu_shared[UUID_IEarm_AccResCPU_Max + 1];

#define IDLE_ACCOUNT

L4_Word64_t debug_pmc[8];

// don't write to the log, because the guest also reads it
//#define LOG_READONLY


void eacc_cpu_pmc_setup()
{
        L4_Word64_t buf;

        // Configure ESCRs

        buf = ((L4_Word64_t)0x0 << 32) | 0xFC00;
        x86_wrmsr(MSR_IA32_TC_PRECISE_EVENT, buf);

        // Enable Precise Event Based Sampling (accurate & low sampling overhead)
        buf = ((L4_Word64_t)0x0 << 32) | 0x1000001;
        x86_wrmsr(MSR_IA32_PEBS_ENABLE, buf);

        // Also enabling PEBS
        buf = ((L4_Word64_t)0x0 << 32) | 0x1;
        x86_wrmsr(MSR_IA32_PEBS_MATRIX_VERT, buf);

        // Count unhalted cycles
        buf = ((L4_Word64_t)0x0 << 32) | 0x2600020C;
        x86_wrmsr(MSR_IA32_FSB_ESCR0, buf);

        // Count load uops that are replayed due to unaligned addresses
        // and/or partial data in the Memory Order Buffer (MOB)
        buf = ((L4_Word64_t)0x0 << 32) | 0x600740C;
        x86_wrmsr(MSR_IA32_MOB_ESCR0, buf);

        // Count op queue writes
        buf = ((L4_Word64_t)0x0 << 32) | 0x12000E0C;
	x86_wrmsr(MSR_IA32_MS_ESCR0, buf);

        // Count retired branches
        buf = ((L4_Word64_t)0x0 << 32) | 0x8003C0C;
        x86_wrmsr(MSR_IA32_TBPU_ESCR0, buf);

        // Unknown
        buf = ((L4_Word64_t)0x0 << 32) | 0x900000C;
	x86_wrmsr(MSR_IA32_FIRM_ESCR0, buf);

        // Count mispredicted
        buf = ((L4_Word64_t)0x0 << 32) | 0x600020C;
	x86_wrmsr(MSR_IA32_CRU_ESCR0, buf);

        // Count memory retired
        buf = ((L4_Word64_t)0x0 << 32) | 0x1000020C;
	x86_wrmsr(MSR_IA32_CRU_ESCR2, buf);

        // Count load miss level 1 data cache
        buf = ((L4_Word64_t)0x0 << 32) | 0x1200020C;
	x86_wrmsr(MSR_IA32_CRU_ESCR3, buf);

        // Unknown
        buf = ((L4_Word64_t)0x0 << 32) | 0x4000C0C;
	x86_wrmsr(MSR_IA32_RAT_ESCR0, buf);

        // Configure CCCRs

        // Store unhalted cycles
        buf = ((L4_Word64_t)0x0 << 32) | 0x3D000;
	x86_wrmsr(MSR_IA32_BPU_CCCR0, buf);

        // Store MOB load replay
        buf = ((L4_Word64_t)0x0 << 32) | 0x35000;
	x86_wrmsr(MSR_IA32_BPU_CCCR1, buf);

        // Storee op queue writes
        buf = ((L4_Word64_t)0x0 << 32) | 0x31000;
	x86_wrmsr(MSR_IA32_MS_CCCR0, buf);

        // Store retired branches
        buf = ((L4_Word64_t)0x0 << 32) | 0x35000;
	x86_wrmsr(MSR_IA32_MS_CCCR1, buf);

        // Store something unknown
        buf = ((L4_Word64_t)0x0 << 32) | 0x33000;
	x86_wrmsr(MSR_IA32_FLAME_CCCR0, buf);

        // Store mispredicted branches
        buf = ((L4_Word64_t)0x0 << 32) | 0x39000;
	x86_wrmsr(MSR_IA32_IQ_CCCR0, buf);

        // Store memory retired
        buf = ((L4_Word64_t)0x0 << 32) | 0x3B000;
	x86_wrmsr(MSR_IA32_IQ_CCCR1, buf);

        // Store load miss level 1 data cache
        buf = ((L4_Word64_t)0x0 << 32) | 0x3b000;
	x86_wrmsr(MSR_IA32_IQ_CCCR2, buf);

        // Store something unknown
        buf = ((L4_Word64_t)0x0 << 32) | 0x35000;
	x86_wrmsr(MSR_IA32_IQ_CCCR4, buf);

        // Setup complete

}

void check_energy_abs(L4_Word_t cpu, L4_Word_t domain)
{
    L4_Word64_t most_recent_timestamp = 0;
    L4_Word64_t exit_pmc[8], entry_pmc[8];
    energy_t idle_energy = 0, access_energy = 0, sum_idle_energy = 0, sum_access_energy = 0;
    L4_Word_t entry_event = 0, exit_event;
    L4_Word_t count = 0;
    
    /*
     * Get logfile
     */
    u16_t *s = L4_SELECTOR(cpu, domain, L4_LOGGING_RESOURCE_CPU);
    logfile_control_t *c = l4_logfile_base[cpu] + (*s / sizeof(logfile_control_t)) ;
    volatile L4_Word_t *current_idx = (L4_Word_t* ) (c + (c->X.current_offset / sizeof(logfile_control_t)));

    printf( "selector %x ctrl reg %x current_idx @ %x = %x\n",
	    s, c, current_idx, *current_idx);

    if (L4_LOGGING_LOG_ENTRIES(c)==0) {
	printf( "No log entries found for CPU %d, break!\n", cpu);
	return;	    
    }

    if (L4_LOGGING_LOG_ENTRIES(c)>2048) {
	printf( "Too many log entries (%d) found! Log may be in corrupt state. Abort!\n",
		L4_LOGGING_LOG_ENTRIES(c));
	return;
    }

    while (count <= (L4_LOGGING_LOG_ENTRIES(c)-32))
    {
	
	/*
	 * Logfile of non-running domain contains 8 entry-exit pairs 
	 * read all 8 exit pairs
	 */
	for (L4_Word_t ctr=0; ctr < 8; ctr++)
	{
	    L4_LOGGING_DEC_LOG(current_idx, c);
	    exit_pmc[7-ctr] = *current_idx;
	    exit_pmc[7-ctr] <<= 32;
	    L4_LOGGING_DEC_LOG(current_idx, c);
	    exit_pmc[7-ctr] += *current_idx;
	}

	//Event = 0 -> exit, 1 -> entry
	exit_event = (L4_Word_t) (exit_pmc[0] & 0x1);; 
	
	if (exit_event)
	{
	    count+=16;
	    continue;
	}
	
	
	if (most_recent_timestamp == 0)
	{
	    //printf( "timestamp " << (void *) (L4_Word_t) exit_pmc[0] << "\n");
	    most_recent_timestamp = exit_pmc[0];;
	}

	/*
	 * Reached stale entries ?
	 */
	if (exit_pmc[0] <= last_acc_timestamp[cpu][domain])
	    break;
	
	/*
	 * read all 8 entry pairs 
	 */
	for (L4_Word_t ctr=0; ctr < 8; ctr++)
	{
	    L4_LOGGING_DEC_LOG(current_idx, c);
	    entry_pmc[7-ctr] = *current_idx;
	    entry_pmc[7-ctr] <<= 32;
	    L4_LOGGING_DEC_LOG(current_idx, c);
	    entry_pmc[7-ctr] += *current_idx;
	}
	
	entry_event = (L4_Word_t) (entry_pmc[0] & 0x1);; 
	
	if (entry_event == 0)
	{
	    printf( "Logfile mismatch entry evt %d / %d dom %d idx %x ct %d sz %d\n",
		    entry_event, L4_LOGGING_RESOURCE_CPU, domain, current_idx, count, 
		    L4_LOGGING_LOG_ENTRIES(c));
	    L4_KDB_Enter("BUG");
	}

	/* Wraparound ? */
	if (exit_pmc[0] <= entry_pmc[0])
	{
	    printf( "wraparound ct %d\n", count);
	    //L4_KDB_Enter("wraparound catcher");
	    break;
	}
	/*
	 * Estimate energy from counters 
	 */
#define DIVISOR 100
	idle_energy = 
	    pmc_weight[0]  * ((exit_pmc[0] - entry_pmc[0])/DIVISOR); // tsc

	access_energy = 0;
	for (L4_Word_t pmc=1; pmc < 8; pmc++)
	    access_energy +=  pmc_weight[pmc] * ((exit_pmc[pmc] - entry_pmc[pmc])/DIVISOR);

	for (L4_Word_t pmc=0; pmc < 8; pmc++)
	    debug_pmc[pmc] +=  ((exit_pmc[pmc] - entry_pmc[pmc])/DIVISOR);


#if defined(IDLE_ACCOUNT)
	sum_idle_energy += idle_energy;
#else
	sum_access_energy += idle_energy;	
	sum_idle_energy = 0;
#endif	
       sum_access_energy += access_energy;
       
       count += 32;
    }
    
    if (vm_t::max_domain_in_use > EACC_MIN_DOMAIN)
	sum_idle_energy /= (vm_t::max_domain_in_use - EACC_MIN_DOMAIN + 1);
    
    sum_access_energy /= 1000;
    sum_idle_energy /= 1000;
    
    last_acc_timestamp[cpu][domain] = most_recent_timestamp;
    manager_cpu_shared[cpu]->clients[domain].access_cost[cpu] += sum_access_energy;
   
    for (L4_Word_t d = EACC_MIN_DOMAIN; d <= vm_t::max_domain_in_use; d++) {
	//manager_cpu_shared[cpu]->clients[d].base_cost += 1000;
       manager_cpu_shared[cpu]->clients[d].base_cost[cpu] += sum_idle_energy;
    }

    //L4_KDB_Enter("Checked energy");

}


/*****************************************************************
 * Module IAccounting
 *****************************************************************/

/* Interface IAccounting::Resource */

IDL4_INLINE void IEarm_AccResource_get_counter_implementation(CORBA_Object _caller, L4_Word_t *hi, L4_Word_t *lo,
							      idl4_server_environment *_env)
{
    /* implementation of IAccounting::Resource::get_counter */
    L4_Word64_t counter = 0;
    L4_Word_t domain = tid_space_t::tid_to_space_id(_caller) + VM_DOMAIN_OFFSET;

    printf( "CPU get_counter dom %d : <%x:%x>\n", domain, *hi, *lo);
    L4_KDB_Enter("Resource_get_counter called");
#if 0   
    for (L4_Word_t cpu = 0; cpu <= max_uuid_cpu; cpu++) {
	check_energy_abs(cpu, domain);

    }

    for (L4_Word_t v = 0; v <= UUID_IEarm_AccResMax; v++) {
	counter += manager_cpu_shared[cpu]->clients[domain].base_cost[v];
	counter + manager_cpu_shared[cpu]->clients[domain].access_cost[v];
    }
#endif
    
    *hi = (counter >> 32);
    *lo = counter;


    return;
}
IDL4_PUBLISH_IEARM_ACCRESOURCE_GET_COUNTER(IEarm_AccResource_get_counter_implementation);

void *IEarm_AccResource_vtable[IEARM_ACCRESOURCE_DEFAULT_VTABLE_SIZE] = IEARM_ACCRESOURCE_DEFAULT_VTABLE;


void IEarm_AccResource_server(
    void *param ATTR_UNUSED_PARAM,
    hthread_t *htread ATTR_UNUSED_PARAM)
{
  L4_ThreadId_t partner;
  L4_MsgTag_t msgtag;
  idl4_msgbuf_t msgbuf;
  long cnt;

  
  max_uuid_cpu = L4_NumProcessors(L4_GetKernelInterface()) - 1;
  if (max_uuid_cpu > UUID_IEarm_AccResCPU_Max)
      max_uuid_cpu = UUID_IEarm_AccResCPU_Max;

  for (L4_Word_t uuid_cpu = 0; uuid_cpu <= max_uuid_cpu; uuid_cpu++)
  {
      /* register with the resource manager */
      eacc_cpu_register( L4_Myself(), uuid_cpu, &manager_cpu_shared[uuid_cpu] );
  }
  idl4_msgbuf_init(&msgbuf);
//  for (cnt = 0;cnt < IEARM_ACCRESOURCE_STRBUF_SIZE;cnt++)
//    idl4_msgbuf_add_buffer(&msgbuf, malloc(8000), 8000);

  while (1)
    {
      partner = L4_nilthread;
      msgtag.raw = 0;
      cnt = 0;

      while (1)
        {
          idl4_msgbuf_sync(&msgbuf);

          idl4_reply_and_wait(&partner, &msgtag, &msgbuf, &cnt);

          if (idl4_is_error(&msgtag))
            break;

          idl4_process_request(&partner, &msgtag, 
			       &msgbuf, &cnt, 
			       IEarm_AccResource_vtable[idl4_get_function_id(&msgtag) & IEARM_ACCRESOURCE_FID_MASK]);
        }
    }
}

void IEarm_AccResource_discard()
{
}


void eacc_cpu_collect()
{
    for (L4_Word_t cpu = 0; cpu <= max_uuid_cpu; cpu++) {
	for (L4_Word_t domain = EACC_MIN_DOMAIN; domain <= vm_t::max_domain_in_use; domain++)
	{
		//printf( "Collector checking CPU energy cpu " << cpu << " domain " << domain << "\n");
	    check_energy_abs(cpu, domain);
	}
    }
}


#if !defined(CONFIG_X_EARM_EAS)
static void eacc_cpu_collector( 
    void *param ATTR_UNUSED_PARAM,
    hthread_t *htread ATTR_UNUSED_PARAM)
{
    L4_Time_t sleep = L4_TimePeriod( EACC_CPU_MSEC * 1000 );

    while (1) {
	eacc_cpu_collect();
	L4_Sleep(sleep);
    }
}
#endif
void eacc_cpu_init()
{
    eacc_cpu_pmc_setup();
    
    
    /* Start resource thread */
    hthread_t *eacc_cpu_thread = get_hthread_manager()->create_thread( 
	hthread_idx_eacc_cpu, 252, false, IEarm_AccResource_server);

    if( !eacc_cpu_thread )
    {
	printf( "EARM: couldn't start cpu accounting manager");
	L4_KDB_Enter();
	return;
    }
    printf( "EARM: cpu accounting manager TID: %t\n", eacc_cpu_thread->get_global_tid());
	    
    eacc_cpu_thread->start();


#if !defined(CONFIG_X_EARM_EAS)
    /* Start collector thread */
    hthread_t *eacc_cpu_col_thread = get_hthread_manager()->create_thread( 
	hthread_idx_eacc_cpu_col, 252, false, eacc_cpu_collector);

    if( !eacc_cpu_col_thread )
    {
	printf( "EARM: couldn't start CPU accounting collector thread");
	L4_KDB_Enter();
	return;
    }
    printf( "EARM: CPU accounting collector TID %t\n", eacc_cpu_col_thread->get_global_tid());
    
    eacc_cpu_col_thread->start();
#endif
}