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


#include <pthread.h>    // Posix threads
#include <stdlib.h>     // free
#include <errno.h>      // errno
#include <string.h>     // strerror
#include <netinet/in.h> // IPPROTO_TCP
#include <sys/types.h>  // defines
#include <sys/socket.h> // defines
#include <netdb.h>      // addrinfo
#include <unistd.h>     // close
#include <fcntl.h>      // fcntl

#include "../include/cb_buffer.h"
#include "../include/db_conn_param.h"
#include "./memc.h"

#define SOCINMEMSIZECLIENT   8192
#define SOCOUTMEMSIZECLIENT  8192
#define SOCLINGERTIMECLIENT  7

static int    memc_send( int sockfd, memc_msg *hdr, memc_extras *ext, uchar **key, ushort keylen, uchar **msg, uint msglen );
static int    memc_recv( int sockfd, memc_msg *hdr, memc_extras *ext, uchar **key, ushort *keylen, int keybuflen, uchar **msg, uint *msglen, int msgbuflen );
static int    memc_get_starting_index( MEMC *cm, char key_last_byte );
static void*  memc_init_thr( void *prm );         // Server calls this before fork (may fork first return later, in parellel)
static int    memc_init_inner( MEMC *cm );
static void*  memc_delete_thr( void *prm );
static void*  memc_quit_thr( void *prm );
static int    memc_close_all( MEMC *cm ); 
static void*  memc_connect_thr( void *prm );      // After IP and port is known, parallel, joined in memc_set and memc_get (and memc_delete, in memc_quit if connected) 
static void*  memc_set_thr( void *prm );          // Parallel
static int    memc_get_seq( MEMC_parameter *pm ); // In sequence
static int    memc_set_common( MEMC *cm, uchar **key, uint keylen, uchar **msg, int msglen, uint cas, ushort vbucketid, ushort expiration, char replace );
static int    memc_join_previous( MEMC *cm );
static int    memc_create_all_sockets( MEMC *cm );
static int    memc_create_socket( MEMC *cm, int indx );
static int    memc_get_any_connection( MEMC *cm );
static int    memc_close_mutexes( MEMC *cm );

static int    memc_get_param( MEMC_parameter **pm );
static int    memc_free_param( MEMC_parameter *pm );

static int    memc_hdr_to_big_endian( memc_msg *hdr );
static int    memc_ext_to_big_endian( memc_extras *ext );


int  memc_free_param( MEMC_parameter *pm ){
	if( pm==NULL ) return CBSUCCESS;
	(*pm).cm = NULL;
	(*pm).key = NULL;
	(*pm).msg = NULL;
	(*pm).ptr1 = NULL;
	(*pm).ptr2 = NULL;
	free( pm );
	pm = NULL;
	return CBSUCCESS;
}

int  memc_get_param( MEMC_parameter **pm ){
	if( pm==NULL ) return CBERRALLOC;
	*pm = (MEMC_parameter *) malloc( sizeof( MEMC_parameter ) );
	if( *pm==NULL ) return CBERRALLOC;
	(**pm).cm = NULL;
	(**pm).key = NULL;
	(**pm).msg = NULL;
	(**pm).keylen = 0;
	(**pm).msglen = 0;
	(**pm).msgbuflen = 0;
	(**pm).cas = 0x00;
	(**pm).dbsindx = 0;	// Index of the connection parameters in '(*cm).sesdbparams'
	(**pm).cindx = 0;	// Index of the connection in '(*(*cn).token).conn'
	(**pm).vbucketid = 0;	// Virtual bucket id (the same number can be used everywhere)
	(**pm).expiration = 0; 	// Set only
	(**pm).ptr1 = NULL;
	(**pm).ptr2 = NULL;
	(**pm).errc = CBSUCCESS;
	(**pm).errg = CBSUCCESS;
	(**pm).special = 0x00; // 1.10.2018, replace
	return CBSUCCESS;
}

int  memc_get_starting_index( MEMC *cm, char last_byte ){
	if( cm==NULL ) return 0;
	return last_byte % (*cm).session_databases;
}

int  memc_connect( MEMC *cm, uchar **key, int keylen ){
	int err = CBSUCCESS, indx = 0;
	char last_byte = 0x00, all_reconnects_failed = 1;
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;
	//13.9.2018, the same function with or without the key: if( key==NULL || *key==NULL ) return CBERRALLOC;

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_CONNECT"); cb_flush_log();

	/*
	 * Wait for the socket initialization (call to 'memc_reinit'). */
	err = memc_join_previous( &(*cm) );
	if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_connect: memc_join_previous, error %i.", err ); }

	/*
	 * Index in session database array. */
	if( key!=NULL && *key!=NULL && keylen>0 ){
		last_byte = (char) (*key)[ keylen-1 ];
		(*(*cm).token).starting_index = memc_get_starting_index( &(*cm), last_byte );
	}
	// otherwice use the previous starting_index

	/* 
	 * All of the redundant connections. */
	for( indx=0; indx<(*cm).redundant_servers_count; ++indx ){
		err = memc_reconnect( &(*cm), indx );
		if( err>=0 )
			all_reconnects_failed = 0; // 7.9.2019
	}
	if( all_reconnects_failed==1 )
		return MEMCERRCONNECT; // 7.9.2018
	return CBSUCCESS;
}

int  memc_reconnect( MEMC *cm, int indx ){
	int err = CBSUCCESS;//, i=0;
	MEMC_parameter *pm = NULL;
	if( cm==NULL || (*cm).token==NULL ) return CBERRALLOC;
	if( indx>=(*cm).redundant_servers_count ) return CBINDEXOUTOFBOUNDS;

	if( (*cm).reinit_in_process==1 ){ // 13.9.2018
		err = pthread_join( (*cm).reinit_thr, NULL); // 13.9.2018 just in case, not needed here if memc_connect is the only function to use
		if( err!=0 ){ cb_clog( CBLOGDEBUG, err, "\nmemc_reconnect: pthread_join %i, errno %i '%s'.", err, errno, strerror( errno ) ); }
	}
	/* Debug 17.8.2018 */
/***
	if( (*cm).session_databases>=1 ){
		cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_connect: getaddrinfo ip '");
		for( i=0; i<(*(*cm).sesdbparams[ 0 ]).iplen; ++i )
			cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*(*cm).sesdbparams[ 0 ]).ip[i] );
		cb_clog( CBLOGDEBUG, CBNEGATION, "' port '");
		for( i=0; i<(*(*cm).sesdbparams[ 0 ]).portlen; ++i )
			cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*(*cm).sesdbparams[ 0 ]).port[i] );
		cb_clog( CBLOGDEBUG, CBNEGATION, "', dbsindx 0" );
	}
	if( (*cm).session_databases>=2 ){
		cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_connect: getaddrinfo ip '");
		for( i=0; i<(*(*cm).sesdbparams[ 1 ]).iplen; ++i )
			cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*(*cm).sesdbparams[ 1 ]).ip[i] );
		cb_clog( CBLOGDEBUG, CBNEGATION, "' port '");
		for( i=0; i<(*(*cm).sesdbparams[ 1 ]).portlen; ++i )
			cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*(*cm).sesdbparams[ 1 ]).port[i] );
		cb_clog( CBLOGDEBUG, CBNEGATION, "', dbsindx 1" );
	}
 ***/

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_RECONNECT"); cb_flush_log();

	/*
	 * One parameter to the thread call. */
	pm = (void*) malloc( sizeof( void* ) ); // 9.8.2017, 31.10.2018
	if( pm==NULL ) return CBERRALLOC;
	memc_get_param( &pm );
	(*pm).cm = &(*cm);
  	//16.9.2018: (*pm).dbsindx = ((*(*cm).token).starting_index + indx)%(*cm).session_databases; // index of the IP and port address to use
  	(*pm).dbsindx = ((*(*cm).token).starting_index + indx + 1)%(*cm).session_databases; // index of the IP and port address to use
	(*pm).cindx = indx; // connection index
cb_clog( CBLOGDEBUG, CBSUCCESS, ", INDX %i, DBINDX %i (reconnect)", (*pm).cindx, (*pm).dbsindx );

	if( (*(*(*cm).token).conn[ indx ]).processing>0 ) return MEMCUNINITIALIZED; // 13.9.2018

	++( (*(*(*cm).token).conn[ indx ]).processing ); // 19.7.2018, 9.8.2018
	(*(*(*cm).token).conn[ (*pm).cindx ]).thr_created = 0;
	err = pthread_create( &( (*(*(*cm).token).conn[ (*pm).cindx ]).thr ), NULL, &memc_connect_thr, pm ); // pointer pm is copied to free it at the end of thread, 8.7.2018
	if( err!=0 ){
              cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_connect: pthread_create cindx %i, error %i, errno %i, '%s'", indx, err, errno, strerror( errno ) );
        }else{
	     (*(*(*cm).token).conn[ (*pm).cindx ]).thr_created = 1;
        }
	return err;
}

void* memc_connect_thr( void *pm ){
	struct addrinfo  hints;

	if( pm==NULL || (* (MEMC_parameter*) pm).cm==NULL || (*(* (MEMC_parameter*) pm).cm).token==NULL ){ // 16.8.2018
	  cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_connect_thr, error %i.", CBERRALLOCTHR );
	  cb_flush_log();
	  pthread_exit( NULL );
	  return NULL;
	}

	/* 13.9.2018, lock the connect mutual execution lock. */
	pthread_mutex_lock( &(*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).mtxconn );

// 30.8.2018, tulee tutkia onko connected==1 ja jos on, suljetaan
// ei toimi rinnakkain

        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        hints.ai_addrlen = 0;            hints.ai_canonname = NULL;
        hints.ai_addr = NULL;            hints.ai_next = NULL;
        hints.ai_flags = ( 0x00 & !AI_PASSIVE ) & AI_NUMERICSERV; // Purpose is to bind address, not to listen
        //24.8.2018 thread safety: hints.ai_flags = hints.ai_flags & AI_NUMERICSERV; // Numeric service field string 9.5.2012
	//AI_ADDRCONFIG; // either IPv4 or IPv6

	/*
	 * 17.8.2018, null terminated strings. */
	(*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).ip[ (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).iplen ] = '\0';
	(*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).port[ (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).portlen ] = '\0';

	/* Debug 17.8.2018 */
	/***
	if( (*(* (MEMC_parameter*) pm).cm).session_databases>=1 ){
		cb_flush_log();
		cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_connect: INDX %i, getaddrinfo ip '", (* (MEMC_parameter*) pm).dbsindx);
		for( i=0; i<(*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).iplen; ++i )
			cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).ip[i] );
		cb_clog( CBLOGDEBUG, CBNEGATION, "' (last: 0x%.2x) port'", (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ].ip[ (*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).iplen ] );
		cb_clog( CBLOGDEBUG, CBNEGATION, " ip length %i.", (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).iplen );
		cb_flush_log();
		for( i=0; i<(*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).portlen; ++i )
			cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).port[i] );
		cb_clog( CBLOGDEBUG, CBNEGATION, "', dbsindx %i (last: 0x%.2x)", (* (MEMC_parameter*) pm).dbsindx, (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (*(* (MEMC_parameter*) pm).dbsindx ]).port[ (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).portlen ] );
		cb_clog( CBLOGDEBUG, CBNEGATION, " port length %i.", (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).portlen );
		cb_flush_log();
	}
	 ***/


	// 8.7.2018: to be checked: ip and port has to be null terminated
	(* (MEMC_parameter*) pm).errg = getaddrinfo( &(* (const char *) (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).ip), \
		&(* (const char *) (*(*(* (MEMC_parameter*) pm).cm).sesdbparams[ (* (MEMC_parameter*) pm).dbsindx ]).port), &hints, &(* (MEMC_parameter*) pm).ptr1 );

	if( (* (MEMC_parameter*) pm).errg!=0 ){
		cb_clog( CBLOGERR, CBNEGATION, "\nmemc_connect_thr: getaddrinfo, error %i '%s'.", (* (MEMC_parameter*) pm).errg, gai_strerror( (* (MEMC_parameter*) pm).errg ) );
	}

/***
if( (*(* (MEMC_parameter*) pm).cm).token!=NULL ){
   if( (*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ].fd<0 )
	cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_connect_thr: error, indx %i fd %i.", (* (MEMC_parameter*) pm).dbsindx, (*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ].fd );
   else
	cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_connect_thr: ok, indx %i fd %i.", (* (MEMC_parameter*) pm).dbsindx, (*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ].fd );
}else{
	cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_connect_thr: (*(* (MEMC_parameter*) pm).cm).token was NULL.");
}
 ***/

	(* (MEMC_parameter*) pm).errc = -1;
	while( (* (MEMC_parameter*) pm).errc<0 && (* (MEMC_parameter*) pm).errg>=0 && (* (MEMC_parameter*) pm).ptr1!=NULL && (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).fd>=0 ) {

		/*
		 * Connect to the remote address. */
		if( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[(* (MEMC_parameter*) pm).cindx]).fd>=0 && (*(* (MEMC_parameter*) pm).ptr1).ai_addr!=NULL ){

// HERE 30.8.2018, suljetaanko ensin jos connected==1
// jos uusi avain, connect olisi tehtava uudelleen

		   /*
	 	    * Check if connected already with the same database index, 30.8.2018. */
		   if( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).connected==1 && \
		       (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).dbsindx!=(*(MEMC_parameter*) pm).dbsindx ){
			//13.9.2018: pthread_mutex_lock( &(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ].mtx );

			/*
			 * Close the previous connection, 30.8.2018. */
	                if( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).fd>=0 ){
	                        close( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).fd ); // Close in server after fork, shutdown at client
	                        (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).fd = -1;
	                }

			/*
			 * Create the new socket, 30.8.2018. */
			(* (MEMC_parameter*) pm).errs = memc_create_socket( &(*(* (MEMC_parameter*) pm).cm), (* (MEMC_parameter*) pm).cindx );
			if( (* (MEMC_parameter*) pm).errs>=CBERROR ){ 
				cb_clog( CBLOGERR, (* (MEMC_parameter*) pm).errs, "\nmemc_connect_thr: memc_create_socket, error %i.", (* (MEMC_parameter*) pm).errs );
			}
			//13.9.2018: pthread_mutex_unlock( &(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ].mtx );
		   }
		   if( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).connected==0 ){ // 30.8.2018
		   	(* (MEMC_parameter*) pm).errc = connect( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).fd, &(*(*(* (MEMC_parameter*) pm).ptr1).ai_addr), (*(* (MEMC_parameter*) pm).ptr1).ai_addrlen );
		   	if( (* (MEMC_parameter*) pm).errc < 0) {
	           	   cb_clog( CBLOGERR, MEMCERRCONNECT, "\nmemc_connect_thr: cindx %i error %i, errno %i, '%s'", (* (MEMC_parameter*) pm).cindx, (* (MEMC_parameter*) pm).errc, errno, strerror( errno ) );
		   	   (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).connected = 0;
		   	   (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).dbsindx = -1; // 30.8.2018
	           	}else{
		   	   (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).connected = 1;
 		   	   (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).dbsindx = (* (MEMC_parameter*) pm).dbsindx; // 30.8.2018
		   	}
		   }
		}else{
			//9.8.2018: if( (*(*(* (MEMC_parameter*) pm).cm).token).conn[(* (MEMC_parameter*) pm).dbsindx].fd<0 )
			if( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[(* (MEMC_parameter*) pm).cindx]).fd<0 )
				cb_clog( CBLOGDEBUG, MEMCERRCONNECT, "\nmemc_connect_thr: fd was %i.", (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[(* (MEMC_parameter*) pm).cindx]).fd );
			if( (*(* (MEMC_parameter*) pm).ptr1).ai_addr==NULL )
				cb_clog( CBLOGDEBUG, MEMCERRCONNECT, "\nmemc_connect_thr: (*(* (MEMC_parameter*) pm).ptr1).ai_addr was NULL.");
			cb_flush_log();
		}

		/*
		 * If not connected, try the next remote address. */
		if( (* (MEMC_parameter*) pm).errc<0 && (* (MEMC_parameter*) pm).errg>=0 && (* (MEMC_parameter*) pm).ptr1!=NULL && (*(* (MEMC_parameter*) pm).ptr1).ai_next!=NULL ){
		   (* (MEMC_parameter*) pm).ptr2 = &(*(*(* (MEMC_parameter*) pm).ptr1).ai_next);
		   (* (MEMC_parameter*) pm).ptr1 = &(*(* (MEMC_parameter*) pm).ptr2);
		}else{
		   (* (MEMC_parameter*) pm).ptr1 = NULL; (* (MEMC_parameter*) pm).ptr2 = NULL;
		}
		//++dbg;
	}
/***
        if( (* (MEMC_parameter*) pm).errc<0 ){
          cb_clog( CBLOGERR, MEMCERRSOCKET, "\nmemc_connect_thr: CONNECT ERROR ERRC %i, errg %i, index %i, dbsindex %i, fd %i, errno %i '%s'.", (* (MEMC_parameter*) pm).errc, (* (MEMC_parameter*) pm).errg, (* (MEMC_parameter*) pm).cindx, (* (MEMC_parameter*) pm).dbsindx, \
	     (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).fd , errno, strerror( errno ));
          (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).lasterr = MEMCERRCONNECT;
        }else{
          cb_clog( CBLOGERR, MEMCERRSOCKET, "\nmemc_connect_thr: CONNECT SUCCESS ERRC %i, errg %i, index %i, dbsindex %i, fd %i, errno %i '%s'.", (* (MEMC_parameter*) pm).errc, (* (MEMC_parameter*) pm).errg, (* (MEMC_parameter*) pm).cindx, (* (MEMC_parameter*) pm).dbsindx, \
	     (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).fd , errno, strerror( errno ));
          (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).lasterr = CBSUCCESS; // 19.8.2018
	}
 ***/

	--( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).processing ); // 9.8.2018
	if( (*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).processing<0 )
		(*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).processing = 0;
	pthread_mutex_unlock( &(*(*(*(* (MEMC_parameter*) pm).cm).token).conn[ (* (MEMC_parameter*) pm).cindx ]).mtxconn ); // 13.9.2018

	memc_free_param( pm );

	cb_flush_log();
	pthread_exit( NULL );

	return NULL; // 10.7.2018
}

int  memc_join_previous( MEMC *cm ){
	int indx = 0, cnt = 0, ret = CBSUCCESS, err = CBSUCCESS;
	int  smallest_connection_error = CBSUCCESS;
	char one_connection_succeeded = 0;
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;


	/*
	 * Reinit, moved here 7.8.2018. */
	if( (*cm).reinit_in_process == 1 ){

		err = pthread_join( (*cm).reinit_thr, NULL);
		// NEVER SET HERE: (*cm).reinit_in_process = 0;
		// SET WHEN ALL THE SOCKETS ARE DONE, 7.8.2018
		if( err!=0 ){ cb_clog( CBLOGERR, CBNEGATION, "\npthread_join (init): indx %i, err %i, errno %i '%s'", indx, err, errno, strerror( errno ) ); }

		/*
		 * Errors preventing the connections to succeed, 19.8.2018. */
		if( (*cm).reinit_err==MEMCERRSOCKET || (*cm).reinit_err==MEMCERRTHREAD || (*cm).reinit_err==MEMCUNINITIALIZED ){ // 19.8.2018
			// (*cm).reinit_err==MEMCERRSOCOPT ){ // 19.8.2018
			cb_clog( CBLOGERR, CBNEGATION, "\npthread_join (init): reinit, error %i.", (*cm).reinit_err );
			//7.9.2018: ret = err;
			ret = (*cm).reinit_err;
		}
	}

	for( indx=0; indx<(*cm).redundant_servers_count && indx<MEMCMAXREDUNDANTDBS; ++indx ){

		/*
		 * All the connections. */
		if( (*(*cm).token).conn!=NULL && (*(*cm).token).conn[ indx ]!=NULL ){
		   if( (*(*(*cm).token).conn[ indx ]).processing != 0 && (*(*(*cm).token).conn[ indx ]).thr!=NULL && \
			(*(*(*cm).token).conn[ indx ]).thr_created==1 ){ // 7.8.2018, 9.8.2018
/***
cb_clog( CBLOGDEBUG, CBNEGATION, "\npthread_join (conn), indx %i", indx );
if( (*(*cm).token).conn[ indx ].thr!=NULL )
	cb_clog( CBLOGDEBUG, CBNEGATION, " thr not null, ");
else
	cb_clog( CBLOGDEBUG, CBNEGATION, " thr null, ");
cb_clog( CBLOGDEBUG, CBNEGATION, "processing %i", (*(*cm).token).conn[ indx ].processing );
cb_flush_log();
 ***/
			err = pthread_join( (*(*(*cm).token).conn[ indx ]).thr, NULL);
			//9.8.2018: (*(*cm).token).conn[ indx ].processing = 0;
			if( err!=0 ){ cb_clog( CBLOGERR, CBNEGATION, "\npthread_join (conn): indx %i, err %i '%s', errno %i '%s'", indx, err, strerror( err ), errno, strerror( errno ) ); }

			if( (*(*(*cm).token).conn[ indx ]).lasterr>=CBERROR ){
				cb_clog( CBLOGERR, CBNEGATION, "\npthread_join (conn): connection %i, error %i.", indx, err );
			}else if( (*(*(*cm).token).conn[ indx ]).lasterr<CBNEGATION ){
				one_connection_succeeded = 1;
			}
			if( indx==0 ){
				/* First. */
				smallest_connection_error = (*(*(*cm).token).conn[ indx ]).lasterr;
			}else if( smallest_connection_error>(*(*(*cm).token).conn[ indx ]).lasterr ){
				/* Smallest. */
				smallest_connection_error = (*(*(*cm).token).conn[ indx ]).lasterr;
			}
		   }
		}

		++cnt;
		if( cnt<(*cm).redundant_servers_count && ( indx>=(*cm).session_databases || indx>=MEMCMAXSESSIONDBS ) ){
			indx = 0; // start from the start of the ring
		}
	}

	if( ret>=CBNEGATION )
		return ret; // reinit error, 19.8.2018
	if( (*cm).redundant_servers_count>(*cm).session_databases )
		return CBINDEXOUTOFBOUNDS;
	if( one_connection_succeeded==0 && smallest_connection_error>=CBNEGATION )
		return smallest_connection_error; // 19.8.2018

	/*
	 * If one connection succeeded, returns CBSUCCESS. Otherwice the smallest of the connection errors.
	 * If reinit fails, 'ret'. 19.8.2018. */
	return CBSUCCESS;
}

int  memc_close_all( MEMC *cm ){
	int indx = 0;
	if( cm==NULL || (*cm).token==NULL ) return CBERRALLOC;
	for( indx=0; indx<(*cm).redundant_servers_count; ++indx ){
		if( (*(*(*cm).token).conn[ indx ]).fd>=0 ){
			close( (*(*(*cm).token).conn[ indx ]).fd ); // Close in server after fork, shutdown at client
			(*(*(*cm).token).conn[ indx ]).fd = -1;
		}
	}
	return CBSUCCESS;
}

int  memc_close_mutexes( MEMC *cm ){
	int indx = 0, errn = 0;
	if( cm==NULL || (*cm).token==NULL ) return CBERRALLOC;

        if( (*cm).token!=NULL ){
                for( indx=0; indx<(*cm).redundant_servers_count; ++indx ){ // 7.9.2018
			if( (*(*(*cm).token).conn[ indx ]).thr!=NULL ){ // 23.10.2018
				errn = pthread_join( (*(*(*cm).token).conn[ indx ]).thr, NULL);
		        	if( errn!=0 && errn!=ESRCH ){ 
					cb_clog( CBLOGDEBUG, errn, "\nmemc_close_mutexes: pthread_join (1), error %i, errno %i '%s'", errn, errno, strerror( errno ) ); 
					if( errn==EDEADLK ) {
						cb_clog( CBLOGDEBUG, errn, ", EDEADLK. A deadlock was detected or the value of thread specifies the calling thread.");
					}
					cb_clog( CBLOGDEBUG, errn, "." ); 
				}
			}
			//if( (*(*(*cm).token).conn[ indx ]).mtx!=PTHREAD_MUTEX_INITIALIZER  ){
			if( (*(*(*cm).token).conn[ indx ]).mtx_created!=0 ){
				(*(*(*cm).token).conn[ indx ]).mtx_created = 0;
	                        errn = pthread_mutex_destroy( &(*(*(*cm).token).conn[ indx ]).mtx );
	                        if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (%i mtx), errno %i '%s'.", indx, errno, strerror( errno ) ); 
				}//else{ (*(*(*cm).token).conn[ indx ]).mtx = PTHREAD_MUTEX_INITIALIZER; }
			}
			//if( (*(*(*cm).token).conn[ indx ]).mtxconn!=PTHREAD_MUTEX_INITIALIZER  ){
			if( (*(*(*cm).token).conn[ indx ]).mtxconn_created!=0  ){
				(*(*(*cm).token).conn[ indx ]).mtxconn_created = 0;
	                        errn = pthread_mutex_destroy( &(*(*(*cm).token).conn[ indx ]).mtxconn );
        	                if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (%i mtxconn), errno %i '%s'.", indx, errno, strerror( errno ) ); 
				}//else{ (*(*(*cm).token).conn[ indx ]).mtxconn = PTHREAD_MUTEX_INITIALIZER; }
			}
                }
        }

	//if( (*cm).send!=PTHREAD_MUTEX_INITIALIZER  ){
	if( (*cm).send_created!=0 ){
		(*cm).send_created = 0;
	        errn = pthread_mutex_destroy( &(*cm).send );
        	if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (1), errno %i '%s'", errno, strerror( errno ) ); 
		}//else{ (*cm).send=PTHREAD_MUTEX_INITIALIZER; }
	}
	//if( (*cm).recv!=PTHREAD_MUTEX_INITIALIZER  ){
	if( (*cm).recv_created!=0 ){
		(*cm).recv_created = 0;
	        errn = pthread_mutex_destroy( &(*cm).recv );
	        if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (2), errno %i '%s'", errno, strerror( errno ) ); 
		}//else{ (*cm).recv=PTHREAD_MUTEX_INITIALIZER; }
	}
	//if( (*cm).set!=PTHREAD_MUTEX_INITIALIZER  ){
	if( (*cm).set_created!=0 ){
		(*cm).set_created = 0;
	        errn = pthread_mutex_destroy( &(*cm).set );
	        if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (3), errno %i '%s'", errno, strerror( errno ) );  
		}//else{ (*cm).set=PTHREAD_MUTEX_INITIALIZER; }
	}
	//if( (*cm).delete!=PTHREAD_MUTEX_INITIALIZER  ){
	if( (*cm).delete_created!=0 ){
		(*cm).delete_created = 0;
	        errn = pthread_mutex_destroy( &(*cm).delete );
	        if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (4), errno %i '%s'", errno, strerror( errno ) );
		}//else{ (*cm).delete=PTHREAD_MUTEX_INITIALIZER; }
	}
	//if( (*cm).quit!=PTHREAD_MUTEX_INITIALIZER  ){
	if( (*cm).quit_created!=0 ){
		(*cm).quit_created = 0;
	        errn = pthread_mutex_destroy( &(*cm).quit );
	        if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (5), errno %i '%s'", errno, strerror( errno ) );
		}//else{ (*cm).quit=PTHREAD_MUTEX_INITIALIZER; }
	}

	if( (*cm).reinit_thr!=NULL && (*cm).reinit_thr_created==1 ){
		errn = pthread_join( (*cm).reinit_thr, NULL); // 23.10.2018
        	if( errn!=0 && errn!=ESRCH ){ // "No such process." errno.h
			cb_clog( CBLOGDEBUG, errn, "\nmemc_close_mutexes: pthread_join (2), error %i, errno %i '%s'", errn, errno, strerror( errno ) );
			if( errn==EDEADLK ) {
				cb_clog( CBLOGDEBUG, errn, ", EDEADLK. A deadlock was detected or the value of thread specifies the calling thread.");
			}
			cb_clog( CBLOGDEBUG, errn, "." );
		}
	}
	//if( (*cm).init!=PTHREAD_MUTEX_INITIALIZER  ){
	if( (*cm).init_created!=0 ){
		(*cm).init_created = 0;
	        errn = pthread_mutex_destroy( &(*cm).init );
	        if( errn!=0 ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_close_mutexes: pthread_mutex_destroy (init), errno %i '%s'", errno, strerror( errno ) );
		}//else{ (*cm).init=PTHREAD_MUTEX_INITIALIZER; }
	}

	return CBSUCCESS;
}

/*
 * With processes, called once in starting process. Otherwice
 * called at start of every thread (cm->token has to be NULL). */
int  memc_init( MEMC *cm ){
	if( cm==NULL ) return CBERRALLOC;

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_INIT"); cb_flush_log();

	(*cm).reinit_in_process = 1;

	return memc_init_inner( &(*cm) );
}
int  memc_init_inner( MEMC *cm ){
	int err = CBSUCCESS;
	if( cm==NULL ) return CBERRALLOC;

	/* Moved here 11.9.2018: */
	//(*cm).send = PTHREAD_MUTEX_INITIALIZER;
	err = pthread_mutex_init( &(*cm).send, NULL );
	if( err!=0 ){ cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_init_inner: pthread_mutex_init, error %i errno %i '%s'.", err, errno, strerror( errno ) ); }
	(*cm).send_created = 1;
	//(*cm).recv = PTHREAD_MUTEX_INITIALIZER;
	err = pthread_mutex_init( &(*cm).recv, NULL );
	if( err!=0 ){ cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_init_inner: pthread_mutex_init, error %i errno %i '%s'.", err, errno, strerror( errno ) ); }
	(*cm).recv_created = 1;
	//(*cm).set = PTHREAD_MUTEX_INITIALIZER;
	err = pthread_mutex_init( &(*cm).set, NULL );
	if( err!=0 ){ cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_init_inner: pthread_mutex_init, error %i errno %i '%s'.", err, errno, strerror( errno ) ); }
	(*cm).set_created = 1;
	//(*cm).delete = PTHREAD_MUTEX_INITIALIZER;
	err = pthread_mutex_init( &(*cm).delete, NULL );
	if( err!=0 ){ cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_init_inner: pthread_mutex_init, error %i errno %i '%s'.", err, errno, strerror( errno ) ); }
	(*cm).delete_created = 1;
	//(*cm).quit = PTHREAD_MUTEX_INITIALIZER;
	err = pthread_mutex_init( &(*cm).quit, NULL );
	(*cm).quit_created = 1;
	if( err!=0 ){ cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_init_inner: pthread_mutex_init, error %i errno %i '%s'.", err, errno, strerror( errno ) ); }
	//(*cm).init = PTHREAD_MUTEX_INITIALIZER;
	err = pthread_mutex_init( &(*cm).init, NULL );
	if( err!=0 ){ cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_init_inner: pthread_mutex_init, error %i errno %i '%s'.", err, errno, strerror( errno ) ); }
	(*cm).init_created = 1;
	/* /Moved */

	(*cm).reinit_in_process = 1;
	//(*cm).reinit_thr = PTHREAD_MUTEX_INITIALIZER;
	err = pthread_create( &(*cm).reinit_thr, NULL, &memc_init_thr, &(*cm) );
	if( err!=0 ){
		(*cm).reinit_thr_created = 0;
              	cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_init_inner: pthread_create, error %i, errno %i '%s'.", err, errno, strerror( errno ) );
		return MEMCUNINITIALIZED; // 7.9.2018
	}
	(*cm).reinit_thr_created = 1;
	return CBSUCCESS;
}
int  memc_wait_all( MEMC *cm ){
	if( cm==NULL || (*cm).token==NULL ) return CBERRALLOC;

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_WAIT_ALL"); cb_flush_log();

	return memc_join_previous( &(*cm) );
}
int  memc_reinit( MEMC *cm ){
	int err = CBSUCCESS;
	if( cm==NULL || (*cm).token==NULL ) return CBERRALLOC;

	(*cm).reinit_in_process = 1;

	err = memc_close_all( &(*cm) );
	if( err>=CBERROR ) return err;

	err = memc_close_mutexes( &(*cm) ); // 11.9.2018
	if( err!=CBSUCCESS ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_reinit: memc_close_mutexes, error %i", err ); }

	return memc_init_inner( &(*cm) );
}
/*
 * Reinit in parallel. Note 10.11.2018: One thread at a time. */
void* memc_init_thr( void *prm ){
	MEMC *cm;
	if( prm==NULL )
		pthread_exit( NULL );
	pthread_mutex_lock( &(* (MEMC*) prm).init );
	cm = &(* (MEMC *) prm);
	if( cm==NULL || (*cm).token==NULL ){
		cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_init_thr: error %i.", CBERRALLOCTHR );
		pthread_mutex_unlock( &(* (MEMC*) prm).init );
		if( cm!=NULL )
			(*cm).reinit_in_process = 0; // 25.8.2018
		pthread_exit( NULL );
		return NULL;
	}
	(*cm).indx = 0; (*cm).err = 0;

	(*cm).reinit_in_process = 1; // 19.7.2018, should be set before pthread_create (doubled here to be sure)

	/*
	 * Allocate MEMC_token (if not a call to 'reinit'). */
	if( (*cm).token==NULL ){
		(*cm).token = (MEMC_token*) malloc( sizeof( MEMC_token ) );
		if( (*cm).token == NULL ){
			(*cm).reinit_err = CBERRALLOC;
			(*cm).reinit_in_process = 0;
			cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_init_thr: error %i.", CBERRALLOCTHR );
			cb_flush_log();
			pthread_mutex_unlock( &(* (MEMC*) prm).init );
			pthread_exit( NULL );
			return NULL;
		}
	}

	/*
	 * Init values. */
	(*(*cm).token).starting_index = 0;
	for( (*cm).indx=0; (*cm).indx<(*cm).redundant_servers_count && (*cm).indx<MEMCMAXREDUNDANTDBS; ++(*cm).indx ){
		(*(*(*cm).token).conn[(*cm).indx]).laststatus = 0x00;
		(*(*(*cm).token).conn[(*cm).indx]).last_thread_status = 0x00;
		(*(*(*cm).token).conn[(*cm).indx]).lasterr = CBSUCCESS;
		(*(*(*cm).token).conn[(*cm).indx]).connected = 0x00;
		(*(*(*cm).token).conn[(*cm).indx]).processing = 0;
		(*(*(*cm).token).conn[(*cm).indx]).fd = -1;
		(*(*(*cm).token).conn[(*cm).indx]).last_cas = 0x00;
		(*(*(*cm).token).conn[(*cm).indx]).thr = NULL; // 7.8.2018
		(*(*(*cm).token).conn[(*cm).indx]).thr_created = 0; // 31.1.2019, Linux
	}

	/*
	 * Create sockets. */
	if( (*cm).server_address_list==NULL ){ 
		(*cm).reinit_err = MEMCADDRESSMISSING; 
		cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_init_thr: error %i.", MEMCADDRESSMISSING );
		cb_flush_log();
		/* 30.8.2018: Continue to create the socket to the defautl address. */
		/***
		(*cm).reinit_in_process = 0;
		pthread_mutex_unlock( &(* (MEMC*) prm).init );
		pthread_exit( NULL );
		return NULL;
		 ***/
	}
	(*cm).err = memc_create_all_sockets( &(*cm) );
        if( (*cm).err>=CBNEGATION ){ // 30.8.2018
		cb_clog( CBLOGERR, MEMCERRSOCKET, "\nmemc_init_thr: No sockets, socket error %i, errno %i '%s'.", \
			(*(*(*cm).token).conn[(*cm).indx]).fd, errno, strerror( errno ));
		(*cm).reinit_err = MEMCERRSOCKET;
		cb_flush_log();
		(*cm).reinit_in_process = 0; // 19.7.2018
		pthread_mutex_unlock( &(* (MEMC*) prm).init );
		pthread_exit( NULL );
        }

	(*cm).reinit_in_process = 0;
	cb_flush_log();
	pthread_mutex_unlock( &(* (MEMC*) prm).init );
	pthread_exit( NULL );
	return NULL;
}
static int    memc_create_all_sockets( MEMC *cm ){
	if( cm==NULL || (*cm).token==NULL ){
		cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_create_all_sockets: error %i.", CBERRALLOC );
		return CBERRALLOC;
	}
	(*cm).err = CBSUCCESS; (*cm).indx = 0; (*cm).some_socket_succeeded = 0;
	for( (*cm).indx=0; (*cm).indx<(*cm).redundant_servers_count; ++(*cm).indx ){ // 30.8.2018
		pthread_mutex_lock( &(*(*(*cm).token).conn[ (*cm).indx ]).mtx );
		(*cm).err = memc_create_socket( &(*cm), (*cm).indx );
		if( (*cm).err<=CBNEGATION )
			(*cm).some_socket_succeeded = 1;
		pthread_mutex_unlock( &(*(*(*cm).token).conn[ (*cm).indx ]).mtx );
	}
	if( (*cm).some_socket_succeeded==0 )
		return (*cm).err;
	return CBSUCCESS;
}
/*
 * Must be locked, 11.10.2018. */
static int    memc_create_socket( MEMC *cm, int indx ){
	int err = CBSUCCESS, one = 1, tmp = 0;
        int socoutmemsize = SOCOUTMEMSIZECLIENT;
        int socinmemsize = SOCINMEMSIZECLIENT;
        struct linger         lng;
        struct addrinfo       hints;
        struct addrinfo      *ptr1 = NULL;
	struct addrinfo      *ptr2 = NULL;
	if( cm==NULL || (*cm).token==NULL ){
		cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_create_socket: error %i.", CBERRALLOC );
		return CBERRALLOC;
	}
        lng.l_onoff = 1;
        lng.l_linger = SOCLINGERTIMECLIENT;

        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        hints.ai_addrlen = 0;            hints.ai_canonname = NULL;
        hints.ai_addr = NULL;            hints.ai_next = NULL;
        hints.ai_flags = ( 0x00 & !AI_PASSIVE ) & AI_NUMERICSERV;    // Purpose is to bind address, not to listen
        //hints.ai_flags = hints.ai_flags & AI_NUMERICSERV; // Numeric service field string 9.5.2012
	//AI_ADDRCONFIG; // either IPv4 or IPv6

	if( (*cm).server_address_list==NULL ){

		/*
		 * Socket, 10.7.2018. */
                (*(*(*cm).token).conn[indx]).fd = socket( PF_UNSPEC, SOCK_STREAM&(0xFF^SOCK_NONBLOCK), IPPROTO_TCP ); // PF_INET
		if( (*(*(*cm).token).conn[indx]).fd>=0 ){
			(*cm).some_socket_succeeded = 1;
			/*
			 * Set as blocking, 19.7.2018. */
			err = fcntl( (*(*(*cm).token).conn[indx]).fd, F_GETFL );
			if( err>=0 ){
				/*
				 * Set as blocking (threads are used with join). */
				err = err & ( ( (int) 0xFF ) | O_NONBLOCK );
				fcntl( (*(*(*cm).token).conn[indx]).fd, F_SETFL, err );
				err = CBSUCCESS;
			}
		}else{
        	  	cb_clog( CBLOGERR, MEMCERRSOCKET, "\nmemc_init_thr: Host IP was not given, socket error %i, errno %i '%s'.", \
				(*(*(*cm).token).conn[indx]).fd, errno, strerror( errno ));
		}
	}else{
		//30.8.2018: for( indx=0; indx<(*cm).redundant_servers_count && (*cm).server_address_list!=NULL; ++indx ){
		(*(*(*cm).token).conn[indx]).fd = -1;
		ptr1 = &(*(*cm).server_address_list);
/***
cb_clog( CBLOGDEBUG, CBSUCCESS, "\nCreating socket, indx %i", indx );
if( ptr1!=NULL )
	cb_clog( CBLOGDEBUG, CBSUCCESS, ", ptr1 not null.");
else
	cb_clog( CBLOGDEBUG, CBSUCCESS, ", ptr1 was null.");
cb_flush_log();
 ***/
		while( ptr1 != NULL && (*(*(*cm).token).conn[indx]).fd<0 ) {

			/*
			 * Socket. */
			(*(*(*cm).token).conn[indx]).fd = socket( (*ptr1).ai_family, (*ptr1).ai_socktype&(0xFF^SOCK_NONBLOCK), (*ptr1).ai_protocol );

			if( (*(*(*cm).token).conn[indx]).fd>=0 ){
				/*
				 * Set as blocking (threads are used with join), 19.7.2018. */
				err = fcntl( (*(*(*cm).token).conn[indx]).fd, F_GETFL );
				if( err>=0 ){
					/*
					 * Set as blocking (threads are used with join). */
					err = err & ( ( (int) 0xFF ) | O_NONBLOCK );
					fcntl( (*(*(*cm).token).conn[indx]).fd, F_SETFL, err );
					err = CBSUCCESS;
				}
			}

			/*
			 * Bind socket to the server address. */
			if( (*(*(*cm).token).conn[indx]).fd>=0 && (*ptr1).ai_addr!=NULL ){
				err = bind( (*(*(*cm).token).conn[indx]).fd, &(*(*ptr1).ai_addr), (*ptr1).ai_addrlen );
				if( err < 0) {
					cb_clog( CBLOGERR, MEMCERRBIND, "\nmemc_init: error %i, errno %i, '%s'", err, errno, strerror( errno ) ); 
				}else{
					(*cm).some_socket_succeeded = 1;
					/*
					 * Set as blocking, 19.7.2018. */
					err = fcntl( (*(*(*cm).token).conn[indx]).fd, F_GETFL );
					if( err>=0 ){
						/*
						 * Set as blocking (threads are used with join). */
						err = err & ( ( (int) 0xFF ) | O_NONBLOCK );
						fcntl( (*(*(*cm).token).conn[indx]).fd, F_SETFL, err );
						err = CBSUCCESS;
					}
				}
			}

			/*
			 * Try next server address. */
			if( (*ptr1).ai_next!=NULL ){
				ptr2 = &(*(*ptr1).ai_next);
				ptr1 = &(*ptr2);
			}else{
				ptr1 = NULL; ptr2 = NULL;
			}
		} // WHILE
		if( (*(*(*cm).token).conn[indx]).fd < 0 ){
			cb_clog( CBLOGERR, MEMCERRSOCKET, "\nmemc_init_thr: socket error %i, errno %i '%s'.", (*(*(*cm).token).conn[indx]).fd, errno, strerror( errno ));
		}

		/*
		 * Socket options. */
		tmp = (*(*(*cm).token).conn[indx]).fd ;
		err = setsockopt( tmp, SOL_SOCKET, SO_RCVBUF, &socinmemsize, sizeof( int ) );
		if( err<0 ){ cb_clog( CBLOGWARNING, MEMCERRSOCOPT, "\nmemc_init_thr: setsockopt SO_RCVBUF, %i, errno %i.", err, errno ); }
		err = setsockopt( tmp, SOL_SOCKET, SO_SNDBUF, &socoutmemsize, sizeof( int ) );
		if( err<0 ){ cb_clog( CBLOGWARNING, MEMCERRSOCOPT, "\nmemc_init_thr: setsockopt SO_SNDBUF, %i, errno %i.", err, errno ); }
		err = setsockopt( tmp, SOL_SOCKET, SO_LINGER, &lng, sizeof( struct linger ) ); // close wait if data in transfer
		if( err<0 ){ cb_clog( CBLOGWARNING, MEMCERRSOCOPT, "\nmemc_init_thr: setsockopt SO_LINGER, %i, errno %i.", err, errno ); }
		err = setsockopt( tmp, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t) sizeof( int ) );
		if( err<0 ) cb_clog( CBLOGWARNING, MEMCERRSOCOPT, "\nmemc_init_thr: setsockopt SO_REUSEADDR returned %i, errno %i '%s'.", err, errno,strerror( errno ) );
		err = setsockopt( tmp, SOL_SOCKET, SO_REUSEPORT, &one, (socklen_t) sizeof( int ) ); // enables duplicate address and port bindings
		if( err<0 ) cb_clog( CBLOGWARNING, MEMCERRSOCOPT, "\nmemc_init_thr: setsockopt SO_REUSEPORT returned %i, errno %i '%s'.", err, errno,strerror( errno ) );
		err = setsockopt( tmp, SOL_SOCKET, SO_RCVBUF, &socinmemsize, (socklen_t) sizeof( int ) );
		if( err<0 ) cb_clog( CBLOGWARNING, MEMCERRSOCOPT, "\nmemc_init_thr: setsockopt SO_REUSEPORT returned %i, errno %i '%s'.", err, errno,strerror( errno ) );

		/*
		 * Set as blocking, 19.7.2018. */
		err = fcntl( (*(*(*cm).token).conn[indx]).fd, F_GETFL );
		if( err>=0 ){
			/*
			 * Set as blocking (threads are used with join). */
			err = err & ( ( (int) 0xFF ) | O_NONBLOCK );
			tmp = fcntl( (*(*(*cm).token).conn[indx]).fd, F_SETFL, err );
			tmp = CBSUCCESS;
			err = CBSUCCESS;
		}
	}
	if( (*cm).some_socket_succeeded==0 )
		return MEMCERRSOCKET;
	return CBSUCCESS;
} // 30.8.2018

int   memc_get_any_connection( MEMC *cm ){
	int indx = 0, err = CBSUCCESS;
	if( cm==NULL || (*cm).token==NULL ) return -1;
	/*
	 * Any ready connection. */
	if( (*cm).reinit_in_process==1 ){
		err = pthread_join( (*cm).reinit_thr, NULL);
		if( err!=0 ){ cb_clog( CBLOGERR, CBNEGATION, "\nmemc_get_any_connection: pthread_join (get any connection): err %i, errno %i '%s'", err, errno, strerror( errno ) ); }
	}
	for( indx=0; indx<(*cm).redundant_servers_count; ++indx ){
		if( (*(*(*cm).token).conn[ indx ]).processing==0 ){
			if( (*(*(*cm).token).conn[ indx ]).connected==1 )
				return indx;
		}
	}
	/*
	 * Any after processing is over starting from first. */
	for( indx=0; indx<(*cm).redundant_servers_count && indx<MEMCMAXREDUNDANTDBS; ++indx ){
		//if( (*(*cm).token).conn[ indx ].processing==1 ){
		if( (*(*(*cm).token).conn[ indx ]).processing!=0 && (*(*(*cm).token).conn[ indx ]).thr!=NULL ){ // 7.8.2018, 9.8.2018
			err = pthread_join( (*(*(*cm).token).conn[ indx ]).thr, NULL);
			if( err!=0 ){ cb_clog( CBLOGERR, CBNEGATION, "\nmemc_get_any_connection: pthread_join (get any connection 2): indx %i, err %i, errno %i '%s'", indx, err, errno, strerror( errno ) ); }
			if( (*(*(*cm).token).conn[ indx ]).connected==1 )
				return indx;
		}else{
			if( (*(*(*cm).token).conn[ indx ]).connected==1 )
				return indx;
		}
	}
	/*
	 * Reconnect, 19.7.2018. */
	for( indx=0; indx<(*cm).redundant_servers_count && indx<MEMCMAXREDUNDANTDBS; ++indx ){
		if( (*(*(*cm).token).conn[ indx ]).processing==0 ){
			err = pthread_join( (*(*(*cm).token).conn[ indx ]).thr, NULL);
			if( err!=0 ){ cb_clog( CBLOGERR, CBNEGATION, "\nmemc_get_any_connection: pthread_join (get any connection 3): indx %i, err %i, errno %i '%s'", indx, err, errno, strerror( errno ) ); }
			if( (*(*(*cm).token).conn[ indx ]).connected==1 )
				return indx;
		}
	}
	return -1;
}

int  memc_get( MEMC *cm, uchar **key, int keylen, uchar **msg, int *msglen, int msgbuflen, uint *cas, ushort vbucketid ){
	int err = CBSUCCESS, cindx = -1, indx = 0;
	char roundfull = 0;
	MEMC_parameter *pm = NULL;
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;
	if( cas==NULL || key==NULL || *key==NULL || msg==NULL || *msg==NULL || cm==NULL ) return CBERRALLOC;

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_GET BUFLEN %i", msgbuflen); cb_flush_log();

	if( keylen<=0 ){
		cb_clog( CBLOGDEBUG, MEMCSENDKEYERR, "\nmemc_get: key may not be empty, error MEMCSENDKEYERR.");
		return MEMCSENDKEYERR; // 21.8.2018
	}

	/*
	 * Wait for the previous data to be updated. */
	cindx = memc_get_any_connection( &(*cm) );
	if( cindx<0 ){
		cb_clog( CBLOGERR, MEMCERRCONNECT, "\nmemc_get: no available connections, error %i.", MEMCERRCONNECT );
	}

	/*
	 * Get empty parameters. */
	pm = (void*) malloc( sizeof( void* ) ); // 9.8.2017, 31.10.2018
	if( pm==NULL ) return CBERRALLOC;
	memc_get_param( &pm ); // 19.7.2018
	if( pm==NULL ) return CBERRALLOC;

	/*
	 * Parameters. */
	(*pm).cm = &(*cm); // 11.7.2018
	(*pm).msg = &(**msg);
	if( *msglen<0 ) return CBOVERFLOW;
	(*pm).msglen = (uint) *msglen;
	(*pm).msgbuflen = msgbuflen;
	(*pm).key = &(**key);
	if( keylen>65536 ) return CBOVERFLOW;
	(*pm).keylen = (ushort) keylen;
	(*pm).vbucketid = vbucketid;
	(*pm).cas = (ushort) *cas;
	(*pm).cindx = cindx;

	/*
	 * Wait for the connections (and the previous data to be updated), 19.7.2018. */
	err = memc_join_previous( &(*cm) );
	if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_get: memc_join_previous, error %i.", err ); }

	/*
	 * Threads are not needed here. */
	err = -1;
	roundfull = 0;
	/* Start from the first known to be available, 30.8.2018 */
	for( indx=cindx; indx<(*cm).redundant_servers_count && err!=MEMCSUCCESS && indx<=MEMCMAXREDUNDANTDBS; ++indx ){ // 30.8.2018
		(*pm).cindx = indx;
		err = memc_get_seq( &(*pm) );
		if( roundfull==0 ){
			if( (indx+1)==(*cm).redundant_servers_count ){
				roundfull = 1;
				if( cindx>0 )
					indx = 0;
			}
		}else if( roundfull==1 && indx==cindx ){
			indx = (*cm).redundant_servers_count; // stop
		}
	}
	if( err==MEMCSUCCESS ){
		*cas = (*pm).cas;
		*msglen = (int) (*pm).msglen;

/***
cb_clog( CBLOGDEBUG, CBNEGATION, "\nFrom memc_get_seq: SUCCESS, msglen %i, status %.2X, msg: [", *msglen, err );
if( (*pm).msg!=NULL ){
  for( indx=0; indx< (int) (*pm).msglen && 0>(int)(*pm).msglen ; ++indx ){ // Debug
	cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (unsigned char) (*pm).msg[ indx ] );
  }
}
cb_clog( CBLOGDEBUG, CBNEGATION, "]"); 
 ***/
	}else{
		; // fail
	}

	/***
	if( (*pm).cindx==((*cm).redundant_servers_count-1) && ( some_was_not_connected==1 || 
	    some_was_connected_and_failed==0 ) ){
		//
		// Last without results. Try reconnecting. 
		//memc_reinit( &(*cm) );
		err = memc_connect( &(*cm), &(*key), keylen );
		if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_get: memc_connect, error %i (2).", err ); }
		err = memc_join_previous( &(*cm) );
		if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_get: memc_join_previous, error %i (2).", err ); }
		cb_clog( CBLOGDEBUG, err, "\nmemc_get: warning, reinit of connections.");
		(*pm).cindx=0;
		continue;
	}
	 ***/

	memc_free_param( pm ); // 19.7.2018
	pm = NULL;
	cb_flush_log();

	return err;
}
int  memc_get_seq( MEMC_parameter *pm ){
	int err = CBSUCCESS; // , indx = 0;
	memc_msg    hdr;
	memc_extras ext;
	if( pm==NULL || (*pm).key==NULL || (*pm).msg==NULL || (*pm).cm==NULL || (*(*pm).cm).token==NULL ) return CBERRALLOC;
	if( (*(*(*pm).cm).token).conn==NULL || (*(*(*pm).cm).token).conn[ (*pm).cindx ]==NULL ) return CBERRALLOC;
	if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd<0 ){
		(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = CBERRFILEOP;
		//1.10.2018: (*(*pm).msg) = NULL;
		(*pm).msg = NULL;
		(*pm).msglen = 0;
		return CBERRFILEOP; // pthread_exit( NULL );
	}
	hdr.magic = MEMCREQUEST; hdr.opcode = MEMCGET; hdr.data_type = MEMCDATATYPE; hdr.vbucket_id = (*pm).vbucketid;
	hdr.key_length = (*pm).keylen;
	hdr.extras_length = 0 ;         // request
	hdr.body_length = (*pm).keylen; // get, 9.8.2018
	hdr.opaque = 0x02; hdr.cas = 0x00;

	if( pm==NULL ) cb_clog( CBLOGDEBUG, CBSUCCESS, " pm NULL ");
	cb_flush_log();
	if( (*pm).cm==NULL ) cb_clog( CBLOGDEBUG, CBSUCCESS, " cm NULL ");
	cb_flush_log();
	if( (*(*pm).cm).token==NULL ) cb_clog( CBLOGDEBUG, CBSUCCESS, " token NULL ");
	cb_flush_log();
	pthread_mutex_lock( &(*(*pm).cm).send );
	err = memc_send( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, NULL, &(*pm).key, (*pm).keylen, NULL, 0 );
	pthread_mutex_unlock( &(*(*pm).cm).send );
	if( err<CBNEGATION ){
		if( (*pm).msgbuflen<0 ) return CBOVERFLOW;
		(*pm).msglen = (uint) (*pm).msgbuflen;
		hdr.body_length = 0;
		hdr.extras_length = 0x04;
		hdr.key_length = 0;
		pthread_mutex_lock( &(*(*pm).cm).recv );
		err = memc_recv( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, &ext, &(*pm).key, &(*pm).keylen, (*pm).keylen, &(*pm).msg, &(*pm).msglen, (*pm).msgbuflen );
		pthread_mutex_unlock( &(*(*pm).cm).recv );
	}
	if( err<CBNEGATION ){
		if( hdr.cas>4294967296 ) return CBOVERFLOW; 
		(*pm).cas = (unsigned int) hdr.cas;
		// Debug
		/***
		cb_clog( CBLOGDEBUG, CBSUCCESS, "\nGet: 10 msg [");
		for( indx=0; indx<(*pm).msglen; ++indx )
				cb_clog( CBLOGDEBUG, CBSUCCESS, "%c", (char) (*pm).msg[indx] ); // incorrect pointer tms. 9.8.2018
		cb_clog( CBLOGDEBUG, CBSUCCESS, "]");
		 ***/
	}else{
		// Debug
		cb_clog( CBLOGDEBUG, CBSUCCESS, "\nGet returned %i, key or value was not found.", err );
		if( err==MEMCSENDEXTERR )
			err = MEMCRECVKEYNOTFOUND;
	}
	cb_flush_log();
	(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = err;
	(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).laststatus = hdr.status;
	if( err!=CBSUCCESS || hdr.status==MEMCSUCCESS ) // 16.9.2018
		return err; // 16.9.2018
	return (int) hdr.status;
}

int  memc_replace( MEMC *cm, uchar **key, int keylen, uchar **msg, int msglen, uint cas, ushort vbucketid, ushort expiration ){
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;
	if( key==NULL || *key==NULL || msg==NULL || *msg==NULL ) return CBERRALLOC;
	if( keylen<0 ) return CBOVERFLOW;
	return memc_set_common( &(*cm), &(*key), (unsigned int) keylen, &(*msg), msglen, cas, vbucketid, expiration, 1 );
}
int  memc_set( MEMC *cm, uchar **key, int keylen, uchar **msg, int msglen, uint cas, ushort vbucketid, ushort expiration ){
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;
	if( key==NULL || *key==NULL || msg==NULL || *msg==NULL ) return CBERRALLOC;
	if( keylen<0 ) return CBOVERFLOW;
	return memc_set_common( &(*cm), &(*key), (unsigned int) keylen, &(*msg), msglen, cas, vbucketid, expiration, 0 );
}
int  memc_set_common( MEMC *cm, uchar **key, uint keylen, uchar **msg, int msglen, uint cas, ushort vbucketid, ushort expiration, char replace ){
	int err = CBSUCCESS, indx = 0, retries = 0;
	char none_succeeded = 1, some_were_not_connected = 1;
	MEMC_parameter *pm = NULL;
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;
	if( key==NULL || *key==NULL || msg==NULL || *msg==NULL ) return CBERRALLOC;

	/*
	 * All at once.
         */

	/*
	 * Wait for the previous data to be updated. */
	err = memc_join_previous( &(*cm) );
	if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_set: memc_join_previous, error %i.", err ); }

	/*
	 * Index in session database array. */
	/*** 16.9.2018
	if( key!=NULL && *key!=NULL && keylen>0 ){
		last_byte = (*key)[ keylen-1 ];
		if( cm!=NULL )
			starting_index = memc_get_starting_index( &(*cm), last_byte );
	}
	 ***/

	/*
	 * Parallel. */
	for( indx=0; indx<(*cm).redundant_servers_count; ++indx ){

		/*
		 * Get empty parameters. */
		pm = (void*) malloc( sizeof( void* ) ); // 9.8.2017, 31.10.2018
		if( pm==NULL ) return CBERRALLOC;
		memc_get_param( &pm );
		if( pm==NULL ) continue;

		/*
		 * Parameters. */
		(*pm).msg = &(**msg);
		if( msglen<0 ) return CBOVERFLOW;
		(*pm).msglen = (unsigned int) msglen;
		(*pm).msgbuflen = msglen;
		(*pm).key = &(**key);
		if( keylen>65536 ) return CBOVERFLOW;
		(*pm).keylen = (unsigned short) keylen;
		(*pm).vbucketid = vbucketid;
		(*pm).expiration = expiration;
		(*pm).cas = cas;
		(*pm).special = 0x00; // replace added 1.10.2018
		if( replace==1 )
			(*pm).special = MEMCREPLACE;

		(*pm).cindx = indx;
		(*pm).dbsindx = (*(*cm).token).starting_index;
		(*pm).cm = &(*cm);


		if( (*(*(*cm).token).conn[ indx ]).connected!=1 ){ // 7.8.2018
			err = memc_join_previous( &(*cm) );
			if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_set_thr: memc_join_previous, error %i (2).", err ); }
		}
		if( (*(*(*cm).token).conn[ indx ]).connected==1 ){
memc_set_retry:
			++((*(*(*cm).token).conn[ indx ]).processing); // 19.7.2018, 9.8.2018
			// ORIG 1.10.2018 (the only one working): 
			//err = pthread_create( &( (*(*(*cm).token).conn[ indx ]).thr ), NULL, &memc_set_thr, pm ); // pointer pm is copied to free it at the end of thread, 8.7.2018
			err = pthread_create( &( (*(*(*cm).token).conn[ indx ]).thr ), NULL, &memc_set_thr, &(*pm) ); // pointer pm is copied to free it at the end of thread, 8.7.2018
			// TEST 1.10.2018: err = pthread_create( &( (*(*(*cm).token).conn[ indx ]).thr ), NULL, &memc_set_thr, &(*pm) ); // pointer pm is copied to free it at the end of thread, 8.7.2018
			if( err!=0 ){
	        	   cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_set_thr: pthread_create cindx %i, error %i, errno %i, '%s'", (*pm).cindx, err, errno, strerror( errno ) );
			   (*(*(*cm).token).conn[ indx ]).last_thread_status = err;
			}else{
			   none_succeeded = 0;
			   (*(*(*cm).token).conn[ indx ]).thr_created = 1;
			}
		}else{
			some_were_not_connected = 1;
		}

		pm = NULL;

		//if( none_succeeded=0 && some_were_not_connected==1 ){
			/*
			 * Detach and try the last one once more.
			 * Let the calling thread continue. */
		//	;
		//}
		if( ( none_succeeded==1 && indx==( (*cm).redundant_servers_count-1 ) ) && retries < 4 ){
			/*
			 * Last without results. Try reconnecting. */
			err = memc_join_previous( &(*cm) );
			if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_set_thr: memc_join_previous, error %i (2).", err ); }
			if( retries==2 ){

				/*
				 * Last chance, something is really wrong, 11.7.2018. */
				err = memc_reinit( &(*cm) );
				if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_set_thr: memc_reinit, error %i (retries %i).", err, retries ); }
				++retries; // 7.8.2018
				goto memc_set_retry;
			}
			/***
			if( err!=MEMCNOTHINGTOJOIN ){
				err = memc_connect( &(*cm), &(*key), keylen );
				if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_set_thr: memc_connect, error %i (2).", err ); }
				err = memc_join_previous( &(*cm) );
				if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_set_thr: memc_join_previous, error %i (2).", err ); }
				cb_clog( CBLOGWARNING, err, "\nmemc_set_thr: warning, reinit of connections.");
			}
			 ***/
			indx = 0;
			++retries;
			continue;
		}
	}
	return CBSUCCESS;
}

void* memc_set_thr( void *prm ){
	memc_msg hdr;
	memc_extras ext;
	MEMC_parameter *pm;
	if( prm==NULL || (* (MEMC_parameter*) prm).cm==NULL ) 
		pthread_exit( NULL );
	pthread_mutex_lock( &(*(* (MEMC_parameter*) prm ).cm).set ); // note 16.9.2018: code quality mutex
	pm = &(* (MEMC_parameter*) prm);
	if( pm==NULL || (*pm).key==NULL ){
	  cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_set_thr, error %i (1).", CBERRALLOCTHR );
	  cb_flush_log();
	  pthread_mutex_unlock( &(*(* (MEMC_parameter*) prm ).cm).set );
	  pthread_exit( NULL );
	  return NULL;
	}
	(*pm).errc = 0; // 10.11.2018
/**
if( (*pm).special==MEMCREPLACE )
  cb_clog( CBLOGDEBUG, CBSUCCESS, "\n *** MEMC_SET_THR (REPLACE) INDX %i, LOCK SET *** ", (*pm).cindx );
else
  cb_clog( CBLOGDEBUG, CBSUCCESS, "\n *** MEMC_SET_THR INDX %i, LOCK SET *** ", (*pm).cindx );
cb_flush_log();
//cb_clog( CBLOGDEBUG, CBSUCCESS, "\n *** MEMC_SET_THR FD, %i *** ", (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd );
 **/
	if( (*pm).msg==NULL || (*pm).cm==NULL || (*(*pm).cm).token==NULL ){
	  //18.8.2018: cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_set_thr, error %i (2) ", CBERRALLOCTHR );
	  cb_clog( CBLOGERR, CBNEGATION, "\nmemc_set_thr, error %i (2) ", CBERRALLOCTHR );
	  if( (*pm).msg==NULL )
		cb_clog( CBLOGDEBUG, CBNEGATION, " (*pm).msg was NULL.");
	  else if( (*pm).cm==NULL )
		cb_clog( CBLOGDEBUG, CBNEGATION, " (*pm).cm was NULL.");
	  else if( (*(*pm).cm).token==NULL )
		cb_clog( CBLOGDEBUG, CBNEGATION, " (*(*pm).cm).token was NULL.");
	  cb_flush_log();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
	  pthread_mutex_unlock( &(*(*pm).cm).set );
#pragma clang diagnostic pop
	  pthread_exit( NULL );
	  return NULL;
	}
	hdr.magic = MEMCREQUEST; hdr.data_type = MEMCDATATYPE; hdr.vbucket_id = (*pm).vbucketid;
	hdr.opcode = MEMCSET; 
	if( (*pm).special==MEMCREPLACE )
		hdr.opcode = MEMCREPLACE;
	hdr.key_length = (*pm).keylen; 
	hdr.extras_length = 8 ; // 8.8.2018, 4 + 4 bytes, flags + expiration
	hdr.body_length = (*pm).msglen ;
	hdr.body_length += hdr.extras_length; // 8.8.2018
	hdr.body_length += hdr.key_length; // 8.8.2018
	hdr.opaque = 0x02; hdr.cas = (*pm).cas;
	ext.flags = 0x00;
	ext.expiration = (*pm).expiration;

/***
if( (*pm).special==MEMCREPLACE )
  cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_SET_THR: MEMC_REPLACE, LEN %i [", (*pm).msglen );
else
  cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_SET_THR: MEMC_SET, LEN %i [", (*pm).msglen );
for( indx=0; indx<(*pm).msglen; ++indx ){
  //1.10.2018: cb_clog( CBLOGDEBUG, CBSUCCESS, "%c", (*(*pm).msg)[ indx ]);
  cb_clog( CBLOGDEBUG, CBSUCCESS, "%c", (*pm).msg[ indx ] );
}
cb_clog( CBLOGDEBUG, CBSUCCESS, "] [");
for( indx=0; indx<(*pm).msglen; ++indx ){
  //1.10.2018: cb_clog( CBLOGDEBUG, CBSUCCESS, "%.2X", (*(*pm).msg)[ indx ]);
  cb_clog( CBLOGDEBUG, CBSUCCESS, "%.2X", (*pm).msg[ indx ]);
}
cb_clog( CBLOGDEBUG, CBSUCCESS, "]");
cb_flush_log();
indx = 0;
 ***/

	pthread_mutex_lock( &(*(*pm).cm).send );
 	//1.10.2018: (*pm).errc = memc_send( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, &ext, &(*(*pm).key), (*pm).keylen, &(*(*pm).msg), (*pm).msglen ); // 9.8.2018
 	(*pm).errc = memc_send( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, &ext, &(*pm).key, (*pm).keylen, &(*pm).msg, (*pm).msglen ); // 9.8.2018
	pthread_mutex_unlock( &(*(*pm).cm).send );

	if( (*pm).errc<CBNEGATION ){
		/*
		 * Receive responce status message. */
		hdr.body_length = 0;
		hdr.extras_length = 0;
		hdr.key_length = 0;
		pthread_mutex_lock( &(*(*pm).cm).recv );
		(*pm).errc = memc_recv( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, NULL, NULL, NULL, 0, NULL, NULL, 0 ); // 9.8.2018
		pthread_mutex_unlock( &(*(*pm).cm).recv );
	}
	if( (*pm).errc<CBNEGATION ){
		if( hdr.cas>4294967296 ) (*pm).errc = CBOVERFLOW;
		(*pm).cas = (unsigned int) hdr.cas;
	}
	(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = (*pm).errc; // 9.8.2018
	(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).laststatus = hdr.status; // 9.8.2018

	--( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing ); // 9.8.2018
	if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing<0 )
		(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing = 0;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
	pthread_mutex_unlock( &(*(*pm).cm).set );
#pragma clang diagnostic pop
	memc_free_param( pm );
	pm = NULL;
	cb_flush_log();
	pthread_exit( NULL );
	return NULL; // 10.7.2018
}

int  memc_delete( MEMC *cm, uchar **key, int keylen, uint cas, ushort vbucketid ){
	int indx = 0, err = CBSUCCESS;
	MEMC_parameter *pm = NULL;
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;
	if( key==NULL || *key==NULL ) return CBERRALLOC;

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_DELETE"); cb_flush_log();

	/*
	 * Join at start if needed. Every redundant server at
	 * the same time. */
	err = memc_join_previous( &(*cm) ); // from connect or from previous command
	if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_delete: memc_join_previous, error %i.", err ); }

	/*
	 * Delete the key from all of the connections. */
	for( indx=0; indx<(*cm).redundant_servers_count; ++indx ){

	   /*
	    * Parameters. */
	   pm = (void*) malloc( sizeof( void* ) ); // 9.8.2017, 31.10.2018
	   if( pm==NULL ) return CBERRALLOC;
	   err = memc_get_param( &pm );
	   if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_delete: memc_get_param, error %i.", err ); }

	   (*pm).cm = &(*cm);
	   (*pm).key = &(**key);
	   if( keylen<0 ) return CBOVERFLOW;
	   if( keylen>65536 ) return CBOVERFLOW;
	   (*pm).keylen = (ushort) keylen;
	   (*pm).msg = NULL;
	   (*pm).msglen = 0;
	   (*pm).msgbuflen = 0;
	   (*pm).cas = cas;
	   (*pm).vbucketid = vbucketid;
	   (*pm).cindx = indx; // 11.8.2018

	   ++( (*(*(*cm).token).conn[ indx ]).processing ); // 19.7.2018, 9.8.2018
	   (*(*(*cm).token).conn[ (*pm).cindx ]).thr_created = 0;
	   err = pthread_create( &(*(*(*cm).token).conn[ indx ]).thr, NULL, &memc_delete_thr, pm ); // pointer pm is copied, 9.7.2018
	   if( err!=0 ){
              cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_delete: pthread_create, error %i, errno %i '%s'.", err, errno, strerror( errno ) );
	   }else{
	      (*(*(*cm).token).conn[ (*pm).cindx ]).thr_created = 1;
	   }
	}
	return CBSUCCESS;
}
void* memc_delete_thr( void *prm ){
	memc_msg hdr;
	MEMC_parameter *pm;
	if( prm==NULL && (* (MEMC_parameter*) prm).cm==NULL ) 
		pthread_exit( NULL );
	pthread_mutex_lock( &(*(* (MEMC_parameter*) prm).cm).set );
	pm = &(* (MEMC_parameter*) prm);
	if( pm==NULL || (*pm).key==NULL ){
	  cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_delete_thr, error %i.", CBERRALLOCTHR );
	  cb_flush_log();
	  if( (*(*pm).cm).token!=NULL ){
	     --( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing ); // 9.8.2018
	     if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing<0 )
	  	(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing = 0;
	  }
	  pthread_exit( NULL );
	  return NULL;
	}
	if( (*pm).cm==NULL || (*(*pm).cm).token==NULL ){
	  cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_delete_thr, error %i.", CBERRALLOCTHR );
	  cb_flush_log();
	  --( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing ); // 9.8.2018
	  if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing<0 )
		(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing = 0;
	  pthread_exit( NULL );
	  return NULL;
	}

	hdr.magic = MEMCREQUEST; hdr.opcode = MEMCDELETE; hdr.data_type = MEMCDATATYPE; hdr.vbucket_id = (*pm).vbucketid;
	hdr.key_length = (*pm).keylen; 
	hdr.extras_length = 0 ; // delete
	hdr.body_length = (*pm).keylen ; // delete
	hdr.opaque = 0x02; hdr.cas = (*pm).cas;

	if( (*(*(*pm).cm).token).conn==NULL || (*(*(*pm).cm).token).conn[ (*pm).cindx ]==NULL ) goto memc_delete_thr_exit; // 31.1.2019

	if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).connected == 1 ){
		pthread_mutex_lock( &(*(*pm).cm).send );
		(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = memc_send( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, NULL, &(*pm).key, (*pm).keylen, NULL, 0 );
		pthread_mutex_unlock( &(*(*pm).cm).send );
		if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr<CBNEGATION ){
			pthread_mutex_lock( &(*(*pm).cm).recv );
			(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = memc_recv( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, NULL, NULL, NULL, 0, NULL, NULL, 0 );
			pthread_mutex_unlock( &(*(*pm).cm).recv );
		}
		--( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing ); // 9.8.2018
		if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing<0 )
			(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing = 0;
	}
memc_delete_thr_exit:
// pointer is copied, thread-safety-analysis, 11.10.2018
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
	pthread_mutex_unlock( &(*(*pm).cm).set );
#pragma clang diagnostic pop
	memc_free_param( pm );
	cb_flush_log();
	pm = NULL;
	pthread_exit( NULL );
	return NULL;
}


/*
 * */
int  memc_quit( MEMC *cm ){
	int indx = 0, err = CBSUCCESS;
	MEMC_parameter *pm = NULL;
	if( cm==NULL ) return CBERRALLOC;
	if( (*cm).token==NULL ) return MEMCUNINITIALIZED;

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_QUIT"); cb_flush_log();

	/*
	 * Wait for the previous data to be updated. */
	err = memc_join_previous( &(*cm) ); // from connect or from previous command
	if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_quit: memc_join_previous, error %i.", err ); }

	/*
	 * Quit each. */
	for( indx=0; indx<(*cm).redundant_servers_count; ++indx ){ // 20.7.2018

	   if( (*(*cm).token).conn==NULL && (*(*cm).token).conn[ (*pm).cindx ]==NULL ){ // 31.1.2019
	     if( (*(*(*cm).token).conn[ indx ]).connected==1 || (*(*(*cm).token).conn[ indx ]).fd>=0  ){

	       /*
	        * Parameters. */
	       pm = (void*) malloc( sizeof( void* ) ); // 9.8.2017, 31.10.2018
	       if( pm==NULL ) return CBERRALLOC;
	       err = memc_get_param( &pm );
	       if( err>=CBERROR ){ cb_clog( CBLOGDEBUG, err, "\nmemc_quit: memc_get_param, error %i.", err ); }

	       (*pm).cm = &(*cm);
	       (*pm).key = NULL;
	       (*pm).keylen = 0;
	       (*pm).msg = NULL;
	       (*pm).msglen = 0;
	       (*pm).msgbuflen = 0;
	       (*pm).cas = 0x00;
	       (*pm).vbucketid = 0x00;
	       (*pm).cindx = indx; // 9.8.2018

	       ++( (*(*(*cm).token).conn[ indx ]).processing ); // 19.7.2018, 9.8.2018
	       (*(*(*cm).token).conn[ (*pm).cindx ]).thr_created = 0;
	       err = pthread_create( &(*(*(*cm).token).conn[ indx ]).thr, NULL, &memc_quit_thr, pm ); // pointer pm is copied, 9.7.2018
	       if( err!=0 ){
                  cb_clog( CBLOGERR, MEMCERRTHREAD, "\nmemc_quit: pthread_create, error %i, errno %i '%s'.", err, errno, strerror( errno ) );
	       }else{
	          (*(*(*cm).token).conn[ (*pm).cindx ]).thr_created = 1;
	       }
	     }else{
	       cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_quit: fail: connected %i, fd %i.", (*(*(*cm).token).conn[ indx ]).connected, (*(*(*cm).token).conn[ indx ]).fd );
	     }
	     pm = NULL; // 20.7.2018
	   }
	}

	return CBSUCCESS;
}
void* memc_quit_thr( void *prm ){
	ssize_t len;
	memc_msg hdr;
	MEMC_parameter *pm = NULL;
	if( prm==NULL || (* (MEMC_parameter*) prm).cm==NULL )
		pthread_exit( NULL );
	pthread_mutex_lock( &(*(* (MEMC_parameter*) prm).cm).quit );
	len = 0; // 11.10.2018
	pm = &(* (MEMC_parameter*) prm);
	if( pm==NULL || (*pm).cm==NULL || (*(*pm).cm).token==NULL ){
	  cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_quit_thr, error CBERRALLOCTHR (1) " );
	  if( pm==NULL ){
		cb_clog( CBLOGDEBUG, CBNEGATION, "pm was null." );
	  }else if( (*pm).cm==NULL ){
		cb_clog( CBLOGDEBUG, CBNEGATION, "cm was null." );
	  }else if( (*(*pm).cm).token==NULL ){
		cb_clog( CBLOGDEBUG, MEMCUNINITIALIZED, "token was null, error MEMCUNINITIALIZED." );
	  }
	  cb_clog( CBLOGERR, CBERRALLOCTHR, "\nmemc_quit_thr, error %i (1) ", CBERRALLOCTHR );
	  cb_flush_log();
	  pthread_mutex_unlock( &(*(* (MEMC_parameter*) prm).cm).quit );
	  pthread_exit( NULL );
	  return NULL;
	}

	if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).connected==1 && (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd>=0 ){ // 20.7.2018
		hdr.magic = MEMCREQUEST; hdr.opcode = MEMCQUIT; hdr.data_type = MEMCDATATYPE; hdr.vbucket_id = (*pm).vbucketid;
		hdr.key_length = (*pm).keylen;
		hdr.extras_length = 0x00 ; // quit
		hdr.body_length = (*pm).msglen ;
		hdr.opaque = 0x02; hdr.cas = (*pm).cas;

		pthread_mutex_lock( &(*(*pm).cm).send );
		(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = memc_send( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, NULL, NULL, 0, NULL, 0 );
		pthread_mutex_unlock( &(*(*pm).cm).send );
		if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr<CBNEGATION ){

			hdr.body_length = 0;
			hdr.extras_length = 0;
			hdr.key_length = 0;
			pthread_mutex_lock( &(*(*pm).cm).recv );
 			(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = memc_recv( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, &hdr, NULL, NULL, NULL, 0, NULL, NULL, 0 );
			pthread_mutex_unlock( &(*(*pm).cm).recv );

		}

		/*
		 * Shutdown connection. */
		len = shutdown( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd, SHUT_RDWR ); 

		/*
		 * Status information. */
		(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).laststatus = hdr.status;
	}else{
		if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd<0 )
			(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = MEMCERRSOCKET; // 19.8.2018
		else
			(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).lasterr = MEMCERRCONNECT; // 19.8.2018
	}
	(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).connected = 0;
	(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).fd = -1;
	--( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing ); // 9.8.2018
	if( (*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing<0 )
		(*(*(*(*pm).cm).token).conn[ (*pm).cindx ]).processing = 0;

// pointer is copied, thread-safety-analysis, 11.10.2018
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
	pthread_mutex_unlock( &(*(*pm).cm).quit );
#pragma clang diagnostic pop

	memc_free_param( pm );
	pm = NULL;

	cb_flush_log();
	pthread_exit( NULL );
	return NULL;
}

/*
 * Converts to 'memcached' big endian format, 8.8.2018. */
int  memc_hdr_to_big_endian( memc_msg *hdr ){ // all shorts, ints and long ints
	ushort new16 = 0;
	uint   new32 = 0;
	uint   tmp32 = 0;
	unsigned long long int new64 = 0;
	if( hdr==NULL ) return CBERRALLOC;
	if( cb_test_cpu_endianness()==CBBIGENDIAN ){ 
		cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_hdr_to_big_endian: was in big endian format, %i.", cb_test_cpu_endianness() );
		return CBSUCCESS; 
	}

	new16 = (ushort) cb_reverse_two_bytes( (uint) (*hdr).key_length ) ;
	(*hdr).key_length = (ushort) new16 ;

	new16 = (ushort) cb_reverse_two_bytes( (uint) (*hdr).status ) ;
	(*hdr).status = (ushort) new16;

	// ends
	new32 = (*hdr).body_length>>24;
	tmp32 = (*hdr).body_length<<24; 
	new64 = ( 0x000000FF & new32 ) | ( 0xFF000000 & tmp32 );
	// middle
	new32 = (*hdr).body_length>>8;
	tmp32 = (*hdr).body_length<<8;
	(*hdr).body_length = ( 0x0000FF00 & new32 ) | ( 0x00FF0000 & tmp32 );
	// together
	new32 = (*hdr).body_length;
	(*hdr).body_length = (unsigned int) new64 | new32 ;
	new32 = (uint) cb_reverse_four_bytes( (uint) (*hdr).opaque ) ;
	(*hdr).opaque = (uint) new32;
	
	new64 = (uint) cb_reverse_four_bytes( (uint) (*hdr).cas ) ;
	tmp32 = (uint) ( (*hdr).cas>>32 ) ;
	new32 = (uint) cb_reverse_four_bytes( (uint) tmp32 ) ;
	(*hdr).cas = new64<<32;
	(*hdr).cas = ( (*hdr).cas ) | new32;

	return CBSUCCESS;
}
int  memc_ext_to_big_endian( memc_extras *ext ){
	ushort new32 = 0;
	if( ext==NULL ) return CBERRALLOC;
	if( cb_test_cpu_endianness()==CBBIGENDIAN ){ 
		cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_ext_to_big_endian: was in big endian format, %i.", cb_test_cpu_endianness() );
		return CBSUCCESS; 
	}

	new32 = (ushort) cb_reverse_four_bytes( (uint) (*ext).flags ) ;
	(*ext).flags = new32;

	new32 = (ushort) cb_reverse_four_bytes( (uint) (*ext).expiration ) ;
	(*ext).expiration = new32;

	return CBSUCCESS;
}

/*
 * Converts header and extras to memcached big endian format. Does not convert
 * the key or the value (because the encoding is not known). 
 *
 *  - Convert your key and value in some appropriate big endian encoding or the
 *    network byte order encoding of the 'memcached' (to use big and little endian 
 *    machines simultaneously with the same system).
 *
 */
int  memc_send( int sockfd, memc_msg *hdr, memc_extras *ext, uchar **key, ushort keylen, uchar **msg, uint msglen ){
	int len = 0, total = 0; // , indx = 0;
	if( sockfd<0 ) return CBERRFILEOP;
	if( hdr==NULL ) return CBERRALLOC;
	memc_hdr_to_big_endian( &(*hdr) );
	//len = send( sockfd, &(*hdr), (size_t) 24, 0x00 );  // header
	len = (int) write( sockfd, &(* (void*) hdr), (size_t) 24 );  // header
	if( len>0 ) total+=len;
	if( len>0 && len!=24 ) return MEMCSENDINVALIDHDRERR;
	if( len>=0 && ext!=NULL ){
		memc_ext_to_big_endian( &(*ext) );
		//len = send( sockfd, &(*ext), (size_t) (*hdr).extras_length, 0x00 );  // extras
		len = (int) write( sockfd, &(* (void*) ext), (size_t) (*hdr).extras_length );  // extras
		if( len>0 ) total+=len;
		if( len!=(*hdr).extras_length ){
			cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_send: write, written extras length %i was not the extras length %i.", len, (int) (*hdr).extras_length );
			return MEMCSENDINVALIDEXTERR;
		}
	}else if( ext!=NULL ){ // 18.7.2018
		return MEMCSENDHDRERR; 
	}
	if( len>=0 && key!=NULL && *key!=NULL && keylen>0 ){
		//len = send( sockfd, &(**key), (size_t) keylen, 0x00 );    // key
		len = (int) write( sockfd, &(**key), (size_t) keylen );    // key
/***
cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_send: write, WROTE %i LENGTH of THE KEY %i [", len, keylen );
if( len>0 ){
	for( indx=0; indx<len; ++indx )
		cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*key)[indx] );
}
cb_clog( CBLOGDEBUG, CBNEGATION, "] [");
if( len>0 ){
	for( indx=0; indx<len; ++indx )
		cb_clog( CBLOGDEBUG, CBNEGATION, "%.2X", (*key)[indx] );
}
cb_clog( CBLOGDEBUG, CBNEGATION, "]");
 ***/
		if( len>0 ) total+=len;
		if( len!=keylen ){
			cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_send: write, written key length %i was not the key length %i.", len, keylen );
			return MEMCSENDINVALIDKEYERR;
		}
	}else if( key!=NULL ){
		return MEMCSENDEXTERR;
	}
	if( len>=0 && msg!=NULL && *msg!=NULL && msglen>0 ){
		//len = send( sockfd, &(**msg), (size_t) msglen, MSG_EOR ); // message completes record (tested: does not work)
		//len = send( sockfd, &(**msg), (size_t) msglen, MSG_EOF ); // message completes transaction
		len = (int) write( sockfd, &(**msg), (size_t) msglen ); // 'message length bytes hopefully sent at this point'
/***
cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_send: write, WROTE %i LENGTH of THE MESSAGE %i [", len, (int) msglen );
if( len>0 ){
	for( indx=0; indx<len; ++indx )
		cb_clog( CBLOGDEBUG, CBNEGATION, "%c", (*msg)[indx] );
}
cb_clog( CBLOGDEBUG, CBNEGATION, "]");
 ***/
		if( len>0 ) total+=len;
		if( len!=(int)msglen ){
			cb_clog( CBLOGDEBUG, CBNEGATION, "\nmemc_send: write, written msg length %i was not the message length %i.", len, (int) msglen );
			return MEMCSENDINVALIDMSGERR;
		}
	}else if( msg!=NULL ){ 
		return MEMCSENDKEYERR; 
	}
	if( len<0 ){ 
		cb_clog( CBLOGDEBUG, CBERRFILEOP, "\nmemc_send:  %i errno %i '%s'.", len, errno, strerror( errno ) ); 
		return MEMCSENDMSGERR; 
	}else if( len!=(int)msglen && msg!=NULL ){ // 18.7.2018
		cb_clog( CBLOGDEBUG, CBERRFILEOP, "\nmemc_send:  could not write the header length data, %i errno %i '%s'.", len, errno, strerror( errno ) ); 
		return MEMCSENDINVALIDMSGERR;
	}
	return CBSUCCESS; // 25.8.2018
}

int  memc_recv( int sockfd, memc_msg *hdr, memc_extras *ext, uchar **key, ushort *keylen, int keybuflen, uchar **msg, uint *msglen, int msgbuflen ){
	int len = 0, total = 0;
	if( sockfd<0 ) return CBERRFILEOP;
	if( hdr==NULL ) return CBERRALLOC;
	if( msglen!=NULL)
		*msglen = 0; // body length is zero before reading anything

	//len = recv( sockfd, &(*hdr), (size_t) 24, MSG_WAITALL );  // header
	len = (int) read( sockfd, &(* (void*) hdr), (size_t) 24 );  // header

/***
cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_recv: STATUS %.4X (vbucket %.4X) ", (*hdr).status, (*hdr).vbucket_id );
memc_print_err( (int) (*hdr).status ); // 30.8.2018
memc_print_err( (int) (*hdr).vbucket_id ); // 30.8.2018, 16.9.2018
 ***/
	memc_hdr_to_big_endian( &(*hdr) ); // to host byte order, 9.8.2018
/***
cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_recv: STATUS %.4X (vbucket %.4X) ", (*hdr).status, (*hdr).vbucket_id );
memc_print_err( (int) (*hdr).status ); // 30.8.2018
memc_print_err( (int) (*hdr).vbucket_id ); // 30.8.2018, 16.9.2018
cb_flush_log();
 ***/
	if( len>=0 ) total+=len;
	if( len>=0 && ext!=NULL ){
		//len = recv( sockfd, &(*ext), (*hdr).extras_length, MSG_WAITALL );  // extras
		len = (int) read( sockfd, &(* (void*) ext), (*hdr).extras_length );  // extras
		if( len>=0 ) total+=len;
		if( (*hdr).extras_length!=len ){
			cb_clog( CBLOGDEBUG, CBERRFILEOP, "\nmemc_recv: header data did not match to the extras length, %i errno %i '%s'.", len, errno, strerror( errno ) );
			return MEMCRECVINVALIDEXTERR;
		}
	}else if(ext!=NULL){
		return MEMCRECVHDRERR;
	}
	if( len>=0 && key!=NULL && *key!=NULL && keylen!=NULL && *keylen>0 && (*hdr).key_length<keybuflen ){
		//len = recv( sockfd, &(**key), (size_t) (*hdr).key_length, MSG_WAITALL );    // key
		len = (int) read( sockfd, &(**key), (size_t) (*hdr).key_length );    // key
		if( len>=0 ){ 
			total+=len;
			if( len>65536 ) return CBOVERFLOW;
			*keylen = (unsigned short) len;
		}else
			*keylen = 0;
		if( *keylen!=(*hdr).key_length ){ 
			cb_clog( CBLOGDEBUG, CBERRFILEOP, "\nmemc_recv: header data did not match to the key length, %i errno %i '%s'.", len, errno, strerror( errno ) ); 
			return MEMCRECVINVALIDKEYERR;
		}
	}else if( key!=NULL && *key!=NULL && keylen!=NULL ){
		return MEMCRECVEXTERR;
	}
	if( len>=0 && msg!=NULL && *msg!=NULL && msglen!=NULL && (*hdr).body_length<(unsigned int)msgbuflen && (*hdr).body_length<2147483648 ){
		//len = recv( sockfd, &(**msg), (size_t) (*hdr).body_length, MSG_WAITALL ); // message
		len = (int) read( sockfd, &(**msg), (size_t) (*hdr).body_length ); // message
		if( len>=0 ){
			total+=len;
			*msglen = (unsigned int) len; // (*hdr).body_length;
		}else
			*msglen = 0;
		if( *msglen!= ( ( (*hdr).body_length - (*hdr).extras_length ) - (*hdr).key_length ) ){ 
			cb_clog( CBLOGDEBUG, CBERRFILEOP, "\nmemc_recv: header data did not match to the message length, %i errno %i '%s'.", len, errno, strerror( errno ) ); 
			return MEMCRECVINVALIDMSGERR;
		}
	}else if( msg!=NULL && *msg!=NULL && msglen!=NULL ){
		return MEMCRECVKEYERR;
	}
	if( len<0 ){ 
		cb_clog( CBLOGDEBUG, CBERRFILEOP, "\nmemc_recv:  %i errno %i '%s'.", len, errno, strerror( errno ) ); 
		return MEMCRECVMSGERR;
	}
	return CBSUCCESS; // 19.8.2018
}

int  memc_allocate( MEMC **cm ){
	int indx = 0;
	MEMC *ptr = NULL;
	db_conn_param *dbp = NULL;
	dbs_conn *dbc = NULL;
	if( cm==NULL ){
		cb_clog( CBLOGERR, CBERRALLOC, "\nmemc_allocate: parameter was null, error %i.", CBERRALLOC );
		return CBERRALLOC;
	}

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_ALLOCATE"); cb_flush_log();

	ptr = (MEMC *) malloc( sizeof( MEMC ) );
	if( ptr==NULL ){ 
		cb_clog( CBLOGERR, CBERRALLOC, "\nmemc_allocate: malloc returned NULL, error %i.", CBERRALLOC );
		return CBERRALLOC;
	}
	*cm = &(*ptr);
	ptr = NULL;
	(**cm).token = NULL;
	(**cm).server_address_list = NULL;
	(**cm).session_timeout = 120; // 2 hours in seconds
	(**cm).session_databases = 0;
	(**cm).redundant_servers_count = 1;
	(**cm).reinit_thr = NULL;
	(**cm).reinit_thr_created = 0;
	(**cm).reinit_err = CBSUCCESS;
	(**cm).reinit_in_process = 0;

	(**cm).err = 0; // 10.11.2018
	(**cm).indx = 0; // 10.11.2018

	//(**cm).send = PTHREAD_MUTEX_INITIALIZER;
	//(**cm).recv = PTHREAD_MUTEX_INITIALIZER;
	//(**cm).set = PTHREAD_MUTEX_INITIALIZER;
	//(**cm).delete = PTHREAD_MUTEX_INITIALIZER;
	//(**cm).quit = PTHREAD_MUTEX_INITIALIZER;
	//(**cm).init = PTHREAD_MUTEX_INITIALIZER;

	(**cm).send_created = 0;
	(**cm).recv_created = 0;
	(**cm).set_created = 0;
	(**cm).delete_created = 0;
	(**cm).quit_created = 0;
	(**cm).init_created = 0;

	(**cm).sesdbparams = ( db_conn_param** ) malloc( (MEMCMAXSESSIONDBS+1) * sizeof( db_conn_param* ) ); // pointer size
	for( indx=0; indx<MEMCMAXSESSIONDBS; ++indx ){ // 8.10.2018
		(**cm).sesdbparams[ indx ] = NULL;
	}
	(**cm).sesdbparams[ MEMCMAXSESSIONDBS ] = NULL; // +1
	for( indx=0; indx<MEMCMAXSESSIONDBS; ++indx ){
		dbp = (db_conn_param*) malloc( sizeof( db_conn_param ) ); // data
		//31.10.2018: (**cm).sesdbparams[ indx ] = (db_conn_param*) malloc( sizeof( db_conn_param ) ); // data
		if( dbp==NULL ){ cb_clog( CBLOGERR, CBERRALLOC, "\nmemc_allocate: malloc, error %i.", CBERRALLOC); return CBERRALLOC; }
		(*dbp).ip = NULL;
		(*dbp).iplen = 0;
		(*dbp).port = NULL;
		(*dbp).portlen = 0;
		(*dbp).modulename = NULL;
		(*dbp).modulenamelen = 0;
		(*dbp).dbconn = NULL;
		(*dbp).encoding = NULL;
		(*dbp).encodinglen = 0;
		(*dbp).fprefix = NULL;
		(*dbp).fprefixlen = 0;
		(*dbp).username = NULL;
		(*dbp).usernamelen = 0;
		(*dbp).password = NULL;
		(*dbp).passwordlen = 0;
		(*dbp).dbname = NULL;
		(*dbp).dbnamelen = 0;
		(*dbp).exec_count = 0;
		(**cm).sesdbparams[ indx ] = &(*dbp); // data
		dbp = NULL;
	}
	/*
	 * Token. */
	(**cm).token = (MEMC_token *) malloc( sizeof( MEMC_token ) );
	if( (**cm).token==NULL ) return CBERRALLOC;
	(*(**cm).token).starting_index = 0;
	(*(**cm).token).conn = (dbs_conn**) malloc( (MEMCMAXREDUNDANTDBS+1) * sizeof( dbs_conn* ) ); // pointer array
	if( (*(**cm).token).conn == NULL ) return CBERRALLOC;
	for( indx=0; indx<MEMCMAXREDUNDANTDBS; ++indx ){ 
		(*(**cm).token).conn[ indx ] = NULL;  
	}
	(*(**cm).token).conn[ MEMCMAXREDUNDANTDBS ] = NULL; // +1
	for( indx=0; indx<MEMCMAXREDUNDANTDBS; ++indx ){
		dbc = (dbs_conn*) malloc( sizeof( dbs_conn ) ); // data
		// 31.10.2018: (*(**cm).token).conn[ indx ] = (dbs_conn*) malloc( sizeof( dbs_conn ) ); // data
		if( dbc==NULL ){ cb_clog( CBLOGERR, CBERRALLOC, "\nmemc_allocate: malloc, error %i (2).", CBERRALLOC); return CBERRALLOC; }
		(*dbc).fd = -1;
		(*dbc).dbsindx = -1;
		(*dbc).thr = NULL; // 7.8.2018
		(*dbc).thr_created = 0; // 31.1.2019, Linux
		(*dbc).last_thread_status = 0;
		(*dbc).last_cas = 0;
		(*dbc).lasterr = 0;
		(*dbc).laststatus = 0;
		(*dbc).connected = 0;
		(*dbc).processing = 0;
		//(*dbc).mtx = PTHREAD_MUTEX_INITIALIZER;
		//(*dbc).mtxconn = PTHREAD_MUTEX_INITIALIZER;
		(*dbc).mtx_created = 0;
		(*dbc).mtxconn_created = 0;
		(*(**cm).token).conn[ indx ] = &(*dbc);
		dbc = NULL;
		if( (*(**cm).token).conn[ indx ] == NULL ) return CBERRALLOC;
	}
	return CBSUCCESS;
}
int  memc_free( MEMC *cm ){
	int errn = 0; //, indx = 0;
	if( cm==NULL ) return CBSUCCESS;

cb_clog( CBLOGDEBUG, CBSUCCESS, "\nMEMC_FREE"); cb_flush_log();

	errn = memc_close_mutexes( &(*cm) ); // 11.9.2018
	if( errn!=CBSUCCESS ){ cb_clog( CBLOGDEBUG, CBSUCCESS, "\nmemc_free: memc_close_mutexes, error %i", errn ); }

	if( (*cm).server_address_list!=NULL ){
		freeaddrinfo( (*cm).server_address_list );
		(*cm).server_address_list = NULL;
	}
	if( (*cm).token!=NULL ){
		free( (*cm).token );
		(*cm).token = NULL;
	}
	free( cm );  //
	cm = NULL;
	return CBSUCCESS;
}

/*
 * Debug. */
void memc_print_err( int err ){ // err.sh
	switch( err ){
  		case MEMCDATAVERSIONERROR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCDATAVERSIONERROR" );
			break;
  		case MEMCRECVHDRERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVHDRERR" );
			break;
  		case MEMCRECVEXTERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVEXTERR" );
			break;
  		case MEMCRECVKEYERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVKEYERR" );
			break;
  		case MEMCRECVMSGERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVMSGERR" );
			break;
  		case MEMCRECVINVALIDHDRERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVINVALIDHDRERR" );
			break;
  		case MEMCRECVINVALIDEXTERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVINVALIDEXTERR" );
			break;
  		case MEMCRECVINVALIDKEYERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVINVALIDKEYERR" );
			break;
  		case MEMCRECVINVALIDMSGERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVINVALIDMSGERR" );
			break;
  		case MEMCSENDHDRERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDHDRERR" );
			break;
  		case MEMCSENDEXTERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDEXTERR" );
			break;
  		case MEMCSENDKEYERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDKEYERR" );
			break;
  		case MEMCSENDMSGERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDMSGERR" );
			break;
  		case MEMCSENDINVALIDHDRERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDINVALIDHDRERR" );
			break;
  		case MEMCSENDINVALIDEXTERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDINVALIDEXTERR" );
			break;
  		case MEMCSENDINVALIDKEYERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDINVALIDKEYERR" );
			break;
  		case MEMCSENDINVALIDMSGERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDINVALIDMSGERR" );
			break;
  		case MEMCNOTHINGTOJOIN:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCNOTHINGTOJOIN" );
			break;
  		case MEMCADDRESSMISSING:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCADDRESSMISSING" );
			break;
  		case MEMCERRBIND:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCERRBIND" );
			break;
  		case MEMCERRSOCKET:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCERRSOCKET" );
			break;
  		case MEMCERRCONNECT:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCERRCONNECT" );
			break;
  		case MEMCERRTHREAD:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCERRTHREAD" );
			break;
  		case MEMCERRSOCOPT:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCERRSOCOPT" );
			break;
		case MEMCRECVINVALIDDATAERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCRECVINVALIDDATAERR" );
			break;
		case MEMCSENDINVALIDDATAERR:
			cb_clog( CBLOGDEBUG, CBNEGATION, "MEMCSENDINVALIDDATAERR" );
			break;
		default:
			cb_clog( CBLOGDEBUG, CBNEGATION, " ERROR %i ", err );
			break;
	}
}
