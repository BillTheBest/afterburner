/*********************************************************************
 *                
 * Copyright (C) 2003-2010,  Karlsruhe University
 *                
 * File path:     vmdump/vmdump.c
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
 * $Id$
 *                
 ********************************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <l4/types.h>
#include <l4/kdebug.h>
#include "hypervisor_idl_client.h"

int main( int argc, char *argv[] )
{
    L4_ThreadId_t tid;
    L4_Word_t space_id;
    L4_Word_t phys_start, phys_size, offset;
    unsigned int transfer_size;
    CORBA_Environment env = idl4_default_environment;
    char *data;
    char *filename;
    int fd;

    if( argc != 4 ) {
	printf( "usage: %s <tid> <filename> <vm id>\n", argv[0] );
	exit(1);
    }

    sscanf( argv[1], "%lx", &tid.raw );
    filename = argv[2];
    sscanf( argv[3], "%lu", &space_id );

    // Get info about the virtual machine.
    IVMControl_get_space_phys_range( tid, space_id, &phys_start, &phys_size, &env );
    if( env._major != CORBA_NO_EXCEPTION )
    {
	CORBA_exception_free( &env );
	printf( "Ipc failure to the working set scanner.\n" );
	exit( 1 );
    }

    printf( "phys start: %p, phys size: %p\n", (void *)phys_start, 
	    (void *)phys_size );

    // Open the output file.
    fd = open( filename, O_RDWR | O_CREAT, 0660 );
    if( fd < 0 ) {
	printf( "Error opening file '%s': %s\n", filename, strerror(errno) );
	exit( 1 );
    }
    // Allocate disk space for the output file.
    if( lseek(fd, phys_size-4, SEEK_SET) == (off_t)-1 ) {
	printf( "Error extending file: %s\n", strerror(errno) );
	exit( 1 );
    }
    if( write(fd, &fd, 4) != 4) {
	printf( "Error extending file: %s\n", strerror(errno) );
	exit( 1 );
    }
    // Map into our memory the output file.
    data = mmap( 0, phys_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if( data == MAP_FAILED ) {
	printf( "Error mapping the file: %s\n", strerror(errno) );
	exit( 1 );
    }


    // Page-in the output file.
    memset( data, 'j', phys_size );

    // Copy the VM.
    fflush(stdout);
    offset = 0;
    while( offset < phys_size )
    {
	transfer_size = phys_size - offset;
	if( transfer_size > 2*1024*1024 )
	    transfer_size = 2*1024*1024;
	IVMControl_get_space_block( tid, space_id, offset, transfer_size,
		&data, &transfer_size, &env );
	if( env._major != CORBA_NO_EXCEPTION )
	{
	    CORBA_exception_free( &env );
	    printf( "Ipc failure.\n" );
	    exit( 1 );
	}

	if( !transfer_size )
	    break;

	offset += transfer_size;
	data += transfer_size;
    }

    munmap( data, phys_size );
    close(fd);

    printf( "%lu bytes of %lu transfered.\n", offset, phys_size );

    if( offset != phys_size )
	exit(1);
    exit(0);
}

