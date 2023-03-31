##### Note 31. March 2023:
Only some tests have been done. The threads have to be quit completely before forking and to be introduced in the process after the process startup. The tool/library is not in any use or under maintenance or development. 

#### Forkable memcached client - beta

'beta' - still in testing. 

Writes values to redundant servers and reads until found. Servers are chosen by the key used in connecting. If the servers 
need to be changed, reconnect is necessary.

- Redundancy - writes a copy to a selected count of servers
- Sharding - chooses the servers with a hash value of the first key

##### How to use 'fork' with threads

Threads with processes, still in testing. To fork, join all the processes and reconnect. Reinit the MEMC before the 
next fork. 

```
int err = 0;
MEMC *mc = NULL;

err = memc_init( &(*mc) );
if(err>MEMCSUCCESS){ /* ... error ... */ }

/*
 * Join all the threads before fork. */
err = memc_wait_all( &(*mc) );
if(err!=MEMCSUCCESS){ /* ... error ... */ }

if( fork()!=0 ){
	/*
	 * Connect or reconnect with 'memc_reinit' */
	err = memc_connect( &(*mc), mykey, mykeylen );
	// err = memc_reinit( &(*mc) );

	/* ... something else ... */

	/*
	 * Send the 'QUIT' command to close the connection. */
	err = memc_quit( &(*mc) );

	memc_free( mc );
	exit(0);
}

/*
 * Get ready for the next one. */
err = memc_reinit( &(*mc) );

/* ... loop back to another ... */

memc_free( mc );
```

A better alternative is to fork a memc client and call the same client with two pipes, another ensures 
the atomic operation, the memc client reads a ticket from the pipe when it is available and another pipe reads the command 
and data. One process only, sequential operation.

##### Installation

Copy 'message' -library *libcb.so* and add it to the library path. Ensure the cb_buffer.h is found in directory '../inlude/. 
Copy the header files from the './ext' -folder to the '../include' . Change the LIBCBPATH variable in the *compile.sh* to 
find the library. Compile.

##### Usage

```
$ ./memc -h

Usage:
	./memc [-g][-s][-d][-q][-h] [ -i <host ip> ] [ -r <number of servers to copy the data> ] \
		 [ -k <key> ] [ -m <data> ] <memcache IP>:<port> [ <memcache2 IP>:<port2> ... ]
	-i	Host IP-address.
	-r	Number of servers to copy the data.
	-k	Key to use to save the value.
	-m	Message to save as a value.
	-g	GET
	-s	SET
	-d	DELETE
	-q	QUIT
	-h	Help.

	Connects to memcache servers and performs the given command with the
	key and data.

```
