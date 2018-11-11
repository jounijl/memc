/*
 * Library to connect to NOSQL databases with 'memcached' binary protocol.
 *
 * Copyright (C) March 2018, November 2018. Jouni Laakso
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 * Neither the name of the copyright owners nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <pthread.h>

#include "./cb_db_module.h"	// db_conn_param

#define MEMCMAXSESSIONDBS    100
#define MEMCMAXREDUNDANTDBS  10

#define ushort	unsigned short
#define uint	unsigned int
#define uchar	unsigned char

/*
 * Connects with hash( key ) value to 'redundant_servers_count' servers
 * and uses these connections with other key values as well, 9.7.2018.
 *
 * Chooses the redundant servers count servers from the servers list with 
 * the key value in the function 'memc_connect'.
 */

/* Memcached responce status values (of the memcached protocol). */
#define MEMCSUCCESS           0x0000
#define MEMCKEYNOTFOUND       0x0001
#define MEMCKEYEXISTS         0x0002
#define MEMCVALUETOOLARGE     0x0003
#define MEMCINVALIDARGUMENTS  0x0004
#define MEMCITEMNOTSTORED     0x0005
#define MEMCNONNUMERICVALUE   0x0006
#define MEMCANOTHERSERVER     0x0007
#define MEMCAUTHERROR         0x0008
#define MEMCAUTHCONTINUE      0x0009
#define MEMCUNKNOWNCOMMAND    0x0081
#define MEMCOUTOFMEMORY       0x0082
#define MEMCNOTSUPPORTED      0x0083
#define MEMCINTERNALERROR     0x0084
#define MEMCBUSY              0x0085
#define MEMCTEMPORARYFAILURE  0x0086

/* Error messages found from errors.conf */
#define MEMCDATAVERSIONERROR     250  // the same in errors.conf, data version mismatch, a race condition
#define MEMCERRCONNECT           407
#define MEMCERRBIND              411

#define MEMCRECVINVALIDDATAERR   610
#define MEMCRECVMSGERR           611
#define MEMCRECVKEYERR           612
#define MEMCRECVHDRERR           613
#define MEMCRECVEXTERR           614
#define MEMCRECVINVALIDHDRERR    615
#define MEMCRECVINVALIDEXTERR    616
#define MEMCRECVINVALIDKEYERR    617
#define MEMCRECVINVALIDMSGERR    618

#define MEMCRECVKEYNOTFOUND      619 // 15.10.2018 to replace 624 in reading

#define MEMCSENDINVALIDDATAERR   620
#define MEMCSENDKEYERR           621
#define MEMCSENDMSGERR           622
#define MEMCSENDHDRERR           623
#define MEMCSENDEXTERR           624
#define MEMCSENDINVALIDKEYERR    625
#define MEMCSENDINVALIDMSGERR    626
#define MEMCSENDINVALIDHDRERR    627
#define MEMCSENDINVALIDEXTERR    628

#define MEMCNOTHINGTOJOIN         40
#define MEMCADDRESSMISSING       600
#define MEMCERRSOCKET            602
#define MEMCERRTHREAD            604
#define MEMCERRSOCOPT            605
#define MEMCUNINITIALIZED        606 // 15.8.2018

/* Command codes */
#define MEMCGET	   		0x00
#define MEMCSET	   		0x01
#define MEMCREPLACE		0x03
#define MEMCDELETE 		0x04
#define MEMCQUIT   		0x07
#define MEMCSASLLIST            0x20
#define MEMCSASLAUTH            0x21
#define MEMCSASLSTEP            0x22

/* Magic */
#define MEMCREQUEST             0x80
#define MEMCRESPONCE            0x81

/* Data type */
#define MEMCDATATYPE            0x00


typedef struct memc_msg {
	uchar                  magic;
	uchar                  opcode;
	ushort                 key_length:16;
	uchar                  extras_length;
	uchar                  data_type;
	union {
	     ushort            status:16;
	     ushort            vbucket_id:16;
	};
	unsigned int           body_length:32;
	unsigned int           opaque:32;
	unsigned long long int cas:64;
} memc_msg;

typedef struct memc_extras {
	uint     flags:32; // Get
	uint     expiration:32; // Get and set
} memc_extras;

typedef struct dbs_conn {
	pthread_t          thr;        // to use in joining the threads (pthread_t is pointer to a structure pthread)
	unsigned long int  last_cas;   // version of the last get data
	pthread_mutex_t    mtx;        // Pointer to structure pthread_mutex
	pthread_mutex_t    mtxconn;
	int                fd;
	int                dbsindx;    // Database number where conneted (to know to reconnect if needed)
	int                last_thread_status;   // last thread status (with process isolation)
	int                lasterr;
	ushort             laststatus;
	char               connected;  // to know if connected
	char               processing; // flag to know if a previous thread is still using the connection (to join with 'thr')
	int                pad64;   // 24.10.2018, 30.10.2018
} dbs_conn;

typedef struct MEMC_token {

	/*
	 * From  0 to redundant_servers_count (maximum is MEMCMAXSESSIONDBS), same order as 'sesdbparams'.
	 * Note: different from 'sesdbparams'. */
	//8.10.2018: dbs_conn           conn[MEMCMAXREDUNDANTDBS]; // individual connections, redundant_server_count.
	dbs_conn         **conn; // individual connections, redundant_server_count.

	/*
	 * Connection data, copied to each process. */
	int                starting_index; // number to use to start connecting/writing (before starting from the beginning redundant_servers_count)

	int                pad64; // 24.10.2018

} MEMC_token;

typedef struct MEMC {

	/*
	 * Every connection uses this data structure. 
	 */

	/*
	 * From token.starting_index to token.starting_index + redundant_servers_count (modulus session_databases). */
        // 7.10.2018: db_conn_param      sesdbparams[MEMCMAXSESSIONDBS];  // after allocating, used as an array: sesdbparams[MEMCMAXSESSIONDBS]
        db_conn_param    **sesdbparams;  // after allocating, used as an array: sesdbparams[MEMCMAXSESSIONDBS]

        /*
         * Number of session databases. */
        int                session_databases;
        int                redundant_servers_count; // 7.6.2018, number of servers to save the data

	/*
	 * Every process receives a copy of this.
	 * Connect after the key value is known.
	 */
	struct addrinfo   *server_address_list; // pointer to res0 pointed memory in rvp_daemon.c, INIT PUUTTUU 7.7.2018

	/*
	 * Common mutexes, 24.8.2018. */
	pthread_mutex_t    send;
	pthread_mutex_t    recv;

	/*
	 * Individual mutexes (thread safety), 24.8.2018. */
	pthread_mutex_t    set;
	pthread_mutex_t    delete;
	pthread_mutex_t    quit;
	pthread_mutex_t    init; // 25.8.2018

	/*
	 * Connections, each caller gets an own copy of this, use NULL. */
	MEMC_token        *token; 		// callers data
	pthread_t          reinit_thr;
	int                reinit_err;
	int                reinit_in_process;

        /*
         * Configuration. */
        signed int         session_timeout; // session timeout in minutes. (cookies need this and the session database)

	/*
	 * Reinit variables to use in functions, 10.11.2018. */
	int                indx;
        int                err;
	int                some_socket_succeeded; // boolean, 10.11.2018

} MEMC;


typedef struct MEMC_parameter {
        unsigned char    *key;          // Parameter key to calculate the index
        MEMC             *cm;           // The same for all
        ushort            keylen;
	ushort            emptypad;     // 10.11.2018
        int               dbsindx;      // Index of the connection parameters in '(*cm).sesdbparams'
        int               cindx;        // Index of the connection in '(*(*cn).token).conn'
// Connect
        uint              cas;          // Data version
        uchar            *msg;          // Message content
        uint              msglen;
        int               msgbuflen;
	struct addrinfo  *ptr1;         // Used in socket and binding to host
        struct addrinfo  *ptr2;         // Used in socket and binding to host
	int               errc;         // Used in connecting
	int               errg;         // Used in connecting
	int               errs;         // Temporary variable used to indicate socket creation result, 31.8.2018
        unsigned short    vbucketid;    // Virtual bucket id (the same number can be used everywhere)
// Get
        unsigned short    expiration;   // Set only
	unsigned char     special;      // Use REPLACE instead if SET or other
	unsigned char     pad[7];
} MEMC_parameter;


/*
 * MEMC has to be allocated before calling the functions.
 */

/*
 * Update starting_index with the key value (use pseudorandom hash here)
 * and connect, either IPv4 or IPv6 address. */
int  memc_connect( MEMC *cm, uchar **key, int keylen ); // Connect to the correct IP addresses from 'sesdbparams' (redundant_servers_count connections)
int  memc_reconnect( MEMC *cm, int indx ); // connect or reconnect to the index connection (at start or at connection failure)
int  memc_init( MEMC *cm ); // allocate connection token array and create sockets (bind) (in a new thread)
/*
 * Reinit closes all the sockets and creates new ones for
 * the next process (called before fork). Init is in a new
 * thread joined at next connect. (20.7.2018 not found
 * anymore: If renew_names==1, calls  getaddrinfo again 
 * for all the addresses.) */
int  memc_reinit( MEMC *cm ); 
int  memc_set( MEMC *cm, uchar **key, int keylen, uchar **msg, int msglen, uint cas, ushort vbucketid, ushort expiration );  // cas is the data version from get, vbucketid is any the same value all the time
int  memc_replace( MEMC *cm, uchar **key, int keylen, uchar **msg, int msglen, uint cas, ushort vbucketid, ushort expiration );  // cas is the data version from get, vbucketid is any the same value all the time
int  memc_get( MEMC *cm, uchar **key, int keylen, uchar **msg, int *msglen, int msgbuflen, uint *cas, ushort vbucketid ); // cas is the data version, vbucketid is any the same value all the time
int  memc_delete( MEMC *cm, uchar **key, int keylen, uint cas, ushort vbucketid );
int  memc_quit( MEMC *cm ); // Send 'quit' to memcached and 'shutdown' all the redundant_servers_count connections

int  memc_allocate( MEMC **cm );
int  memc_free( MEMC *cm );

/* The call is not be needed before memc_init, memc_set, memc_delete or memc_quit. */
int  memc_wait_all( MEMC *cm );

/* Debug printing. */
void memc_print_err( int err );

/***

[ https://github.com/memcached/memcached/wiki/BinaryProtocolRevamped#magic-byte ]

Header:

     Byte/     0       |       1        |       2       |       3       |
        /              |                |               |               |
       |0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7 |0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|
       +---------------+----------------+---------------+---------------+
      0| Magic         | Opcode         | Key length                    |
       +---------------+----------------+---------------+---------------+
      4| Extras length | Data type 0x00 | vbucket id                    |
       +---------------+----------------+---------------+---------------+
      8| Total body length                                              |
       +---------------+----------------+---------------+---------------+
     12| Opaque                                                         |
       +---------------+----------------+---------------+---------------+
     16| CAS                                                            |
       |                                                                |
       +---------------+----------------+---------------+---------------+
       Total 24 bytes

	Magic = 0x80 request, 0x81 responce
	Data type = 0x00
	Opaque = any string copied back in the responce

Message:

     Byte/     0       |       1       |       2       |       3       |
        /              |               |               |               |
       |0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|
       +---------------+---------------+---------------+---------------+
      0/ HEADER                                                        /
       /                                                               /
       /                                                               /
       /                                                               /
       +---------------+---------------+---------------+---------------+
     24/ COMMAND-SPECIFIC EXTRAS (as needed)                           /
      +/  (note length in the extras length header field)              /
       +---------------+---------------+---------------+---------------+
      m/ Key (as needed)                                               /
      +/  (note length in key length header field)                     /
       +---------------+---------------+---------------+---------------+
      n/ Value (as needed)                                             /
      +/  (note length is total body length header field, minus        /
      +/   sum of the extras and key length body fields)               /
       +---------------+---------------+---------------+---------------+
       Total 24 bytes

	n = 24 bytes + 'Extras length' + 'Key length'
	length =  'Total body length' - 'Extras length' - 'Key length'

Responce status location:

     Byte/     0       |       1       |       2       |       3       |
        /              |               |               |               |
       |0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|
       +---------------+---------------+---------------+---------------+
      0| Magic         | Opcode        | Key Length                    |
       +---------------+---------------+---------------+---------------+
      4| Extras length | Data type     | Status                        |
       +---------------+---------------+---------------+---------------+
      8| Total body length                                             |
       +---------------+---------------+---------------+---------------+
     12| Opaque                                                        |
       +---------------+---------------+---------------+---------------+
     16| CAS                                                           |
       |                                                               |
       +---------------+---------------+---------------+---------------+
       Total 24 bytes

	Status = index 6 and 7, bytes 7 and 8

 ***/

