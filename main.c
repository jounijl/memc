#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>     // memset
#include <errno.h>      // errno in realpath
#include <fcntl.h>      // open
#include <sys/param.h>  // MAXPATHLEN
#include <sys/types.h>  // getaddrinfo
#include <sys/socket.h> // getaddrinfo
#include <netdb.h>      // getaddrinfo
#include <netinet/in.h> // IPPROTO_TCP


#include "../include/ipvxformat.h"
#include "../include/get_option.h"
#include "../include/cb_buffer.h"
#include "./memc.h"

#define MEMCPORTLEN 10

#define EXPIRATION  120

int  main( int argc, char *argv[] );
static int get_ip_and_port(unsigned char **ip, int *iplen, unsigned char **port, int *portlen, char *ipandport[], int len);

void usage( char *progname[] );

void usage (char *progname[]){
        fprintf(stderr,"Usage:\n");
        fprintf(stderr,"\t%s [-g][-s][-d][-q][-h] [ -i <host ip> ] [ -r <number of servers to copy the data> ] \\\n", progname[0]);
        fprintf(stderr,"\t\t [ -k <key> ] [ -m <data> ] <memcache IP>:<port> [ <memcache2 IP>:<port2> ... ]\n");
        fprintf(stderr,"\t-i\tHost IP-address.\n");
        //fprintf(stderr,"\t-p\tHost port number.\n");
        fprintf(stderr,"\t-r\tNumber of servers to copy the data.\n");
	fprintf(stderr,"\t-k\tKey to use to save the value.\n");
	fprintf(stderr,"\t-m\tMessage to save as a value.\n");
        fprintf(stderr,"\t-g\tGET\n");
        fprintf(stderr,"\t-s\tSET\n");
        fprintf(stderr,"\t-d\tDELETE\n");
        //fprintf(stderr,"\t-l\tSASL List\n");
        fprintf(stderr,"\t-q\tQUIT\n");
        fprintf(stderr,"\t-h\tHelp.\n");
        fprintf(stderr,"\n\tConnects to memcache servers and performs the given command with the\n");
        fprintf(stderr,"\tkey and data.\n" );
}

#define MESSAGELEN	(10*MAXPATHLEN)

int  main( int argc, char *argv[] ){
	int fromend = 0, atoms = 0, i = 0, indx = 0, num = 0, u = 0, err = CBSUCCESS;
	uint cas = 0;
	char *str_err=NULL;
	const char *value  = NULL;
	struct addrinfo  hints;
	char  hostipset = 0;
	char  hostportset = 0;
	char  cmd = MEMCGET;
        unsigned char  hostipdata[ MAXPATHLEN+1 ];
        unsigned char *hostip=NULL;
	int            hostiplen=0; // hostip and port
        unsigned char  hostportdata[ MEMCPORTLEN+1 ];
        unsigned char *hostport=NULL;
        int            hostportlen=0;
        unsigned char  ipdata[ MAXPATHLEN+1 ]; // before malloc
        unsigned char *ip=NULL;
	int            iplen=0; // ip and port
        unsigned char  portdata[ MEMCPORTLEN+1 ];
        unsigned char *port=NULL;
	int            portlen=0;
        unsigned char  keydata[ MAXPATHLEN+1 ];
        unsigned char *key=NULL;
	int            keylen=0;
        //unsigned char  msgdata[ MAXPATHLEN+1 ];
        unsigned char  msgdata[ MESSAGELEN+1 ];
        unsigned char *msg=NULL;
	int            msglen=0;
	MEMC *cm;
        str_err=NULL;

        //hints.ai_family = PF_UNSPEC;     hints.ai_flags = 0;
        hints.ai_family = PF_INET;       hints.ai_flags = 0;
        hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        hints.ai_addrlen = 0;            hints.ai_canonname = NULL;
        hints.ai_addr = NULL;            hints.ai_next = NULL;
        hints.ai_flags = hints.ai_flags & !AI_PASSIVE; // Purpose is to bind address, not to listen
        hints.ai_flags = hints.ai_flags & AI_NUMERICSERV; // Numeric service field string 9.5.2012
        //AI_ADDRCONFIG; // either IPv4 or IPv6

	// MEMC
	err = memc_allocate( &cm );
	if( err>=CBERROR ){
	  fprintf( stderr, "\nmemc_allocate, error %i, errno %i '%s'.", err, errno, strerror( errno ) );
	  exit( err );
	}

        // Host port text
        memset( &hostportdata[0], 0x20, (size_t) MEMCPORTLEN );
        hostportdata[ MEMCPORTLEN ] = '\0';
        hostport = &hostportdata[0];

        // Host IP address text
        memset( &hostipdata[0], 0x20, (size_t) MAXPATHLEN );
        hostipdata[ MAXPATHLEN ]='\0';
        hostip = &hostipdata[0];

        // Port text
        memset( &portdata[0], 0x20, (size_t) MEMCPORTLEN );
        portdata[ MEMCPORTLEN ] = '\0';
        port = &portdata[0];

        // IP address text
        memset( &ipdata[0], 0x20, (size_t) MAXPATHLEN );
        ipdata[ MAXPATHLEN ]='\0';
        ip = &ipdata[0];

        // Key text
        memset( &keydata[0], 0x20, (size_t) MAXPATHLEN );
        keydata[ MAXPATHLEN ]='\0';
        key = &keydata[0];

        // Msg text
        //memset( &msgdata[0], 0x20, (size_t) MAXPATHLEN );
        memset( &msgdata[0], 0x20, (size_t) MESSAGELEN );
        //msgdata[ MAXPATHLEN ]='\0';
        msgdata[ MESSAGELEN ]='\0';
        msg = &msgdata[0];

        atoms=argc;
        if( argv[(atoms-1)]==NULL && argc>0 )
          --atoms;

        //
        // From end, one field
        fromend = atoms-1;

        /*
         * Memcached ip and port. */
        if ( atoms >= 2 ){
	  i = -1;
	  while( fromend>=2 && strncmp(argv[fromend],"-",1)!=0 && i<IPUFERROR && num<MEMCMAXSESSIONDBS ){
            i = IPUFERROR;
            if( strncmp(argv[fromend],"-",1)!=0 && strchr(argv[fromend],(int)':')!=NULL ){
              i = get_ip_and_port( &ip, &iplen, &port, &portlen, &argv[fromend], (int) strlen(argv[fromend]) );
            }
            if( i < IPUFERROR ){ // ok
              --fromend;
	      if( iplen>=MAXPATHLEN ){ 
		fprintf( stderr, "\nIP length %i was over the limit %i.", iplen, MAXPATHLEN );
		exit(-1); 
	      }
	      (*(*cm).sesdbparams[ num ]).ip = (uchar *) malloc( (size_t) sizeof( char ) * ( iplen + 1 ) ); // db_conn_param
	      for( indx=0; indx<iplen && indx<(MAXPATHLEN-1) ; ++indx ){
		(*(*cm).sesdbparams[ num ]).ip[ indx ] = ip[ indx ];
	      }
	      (*(*cm).sesdbparams[ num ]).ip[ indx ] = '\0';
	      (*(*cm).sesdbparams[ num ]).iplen = indx;
	      if( portlen>=MEMCPORTLEN ){
		fprintf( stderr, "\nPort length %i was over the limit %i.", portlen, MEMCPORTLEN );
		exit(-1); 
	      }
	      (*(*cm).sesdbparams[ num ]).port = (uchar *) malloc( (size_t) sizeof( char ) * ( portlen + 1 ) );
	      for( indx=0; indx<portlen && indx<(MAXPATHLEN-1) ; ++indx ){
		(*(*cm).sesdbparams[ num ]).port[ indx ] = port[ indx ];
	      }
	      (*(*cm).sesdbparams[ num ]).port[ indx ] = '\0';
	      (*(*cm).sesdbparams[ num ]).portlen = indx;
	      ++num;
            }else{
              ip=NULL; iplen=0; // ip was not regognized
            }
	  }
        }
	(*cm).redundant_servers_count = 1;
	(*cm).session_databases = num;
	num = 0;

//fprintf( stderr, "\nAtoms %i", atoms );
	if( atoms<3 ){
	  usage( &(argv[0]) );
          exit( CBSUCCESS );
	}

        // Parameters
        for( i=1 ; i<fromend ; ++i ){ 
          u = get_option( argv[i], argv[i+1], 'i', &value ); // hostip
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
            if( value!=NULL && strlen( (char*) value )>0 ){
                hostiplen = (int) strnlen( &(* (const char *) value) , (size_t) MAXPATHLEN );
                for( u=0; u<MAXPATHLEN && u<hostiplen; ++u )
                   hostipdata[ u ] = (uchar) value[ u ];
		hostipdata[ u ] = '\0';
		hostipset = 1;
            }else{
                fprintf( stderr, "\nHostip igored, length was zero or negative." );
            }
            continue;
          }
          u = get_option( argv[i], argv[i+1], 'p', &value ); // hostport
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
            if( value!=NULL && strlen( (char*) value )>0 ){
                hostportlen = (int) strnlen( &(* (const char *) value) , (size_t) MEMCPORTLEN );
                for( u=0; u<MEMCPORTLEN && u<hostportlen; ++u )
                   hostportdata[ u ] = (uchar) value[ u ];
		hostportdata[ u ] = '\0';
		hostportset = 1;
            }else{
                fprintf( stderr, "\nHostport igored, length was zero or negative." );
            }
            continue;
          }
          u = get_option( argv[i], argv[i+1], 'k', &value ); // hostport
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
            if( value!=NULL && strlen( (char*) value )>0 ){
                keylen = (int) strnlen( &(* (const char *) value) , (size_t) MAXPATHLEN );
                for( u=0; u<MAXPATHLEN && u<keylen; ++u )
                   keydata[ u ] = (uchar) value[ u ];
		keydata[ u ] = '\0';
            }else{
                fprintf( stderr, "\nKey igored, length was zero or negative." );
            }
            continue;
          }
          u = get_option( argv[i], argv[i+1], 'm', &value ); // hostport
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
            if( value!=NULL && strlen( (char*) value )>0 ){
                msglen = (int) strnlen( &(* (const char *) value), (size_t) MESSAGELEN ); // MAXPATHLEN );
                //for( u=0; u<MAXPATHLEN && u<msglen; ++u )
                for( u=0; u<MESSAGELEN && u<msglen; ++u )
                   msgdata[ u ] = (uchar) value[ u ];
		msgdata[ u ] = '\0';
            }else{
                fprintf( stderr, "\nmsg igored, length was zero or negative." );
            }
            continue;
          }
          u = get_option( argv[i], argv[i+1], 'r', &value );  // redundant servers count (all of the mirrored servers together)
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
            if( value!=NULL && strlen( (char*) value )>0 ){
		(*cm).redundant_servers_count = (int) strtol( ( (const char *) value), &str_err, 10);
            }else{
                fprintf( stderr, "\nKey igored, length was zero or negative." );
            }
            continue;
          }
          u = get_option( argv[i], NULL, 'g', &value );
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
	    cmd = MEMCGET;
            continue;
          }
          u = get_option( argv[i], NULL, 'd', &value );
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
	    cmd = MEMCDELETE;
            continue;
          }
          u = get_option( argv[i], NULL, 'q', &value );
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
	    cmd = MEMCQUIT;
            continue;
          }
          u = get_option( argv[i], NULL, 'l', &value );
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
	    cmd = MEMCSASLLIST;
            continue;
          }
          u = get_option( argv[i], NULL, 's', &value );
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
	    cmd = MEMCSET;
            continue;
          }
          u = get_option( argv[i], NULL, 'h', &value );
          if( u == GETOPTSUCCESS || u == GETOPTSUCCESSATTACHED || u == GETOPTSUCCESSPOSSIBLEVALUE ){
	    usage( &(argv[0]) );
            exit( CBSUCCESS );
          }
	}

//fprintf( stderr, "\nPort [%s] hostport [%s]", port, hostport );
//fprintf( stderr, "\nSession databases %i.", (*cm).session_databases );


	/*
	 * Redundant servers. */
	if( (*cm).session_databases < (*cm).redundant_servers_count ){
		fprintf( stderr, "\nNumber of session databases was smaller than the redundant servers count, using the number of session databases." );
		(*cm).redundant_servers_count = (*cm).session_databases;
	}

	/*
	 * Host address. */
	if( hostipset==0 ){
		if( hostportset==1 )
			err = getaddrinfo( NULL, &(* (const char *) hostport), &hints, &(*cm).server_address_list );
		else
			(*cm).server_address_list = NULL;
			//err = getaddrinfo( NULL, NULL, &hints, &(*cm).server_address_list );
	}else{
		if( hostportset==0 )
			err = getaddrinfo( &(* (const char *) hostip), NULL, &hints, &(*cm).server_address_list );
		else
			err = getaddrinfo( &(* (const char *) hostip), &(* (const char *) hostport), &hints, &(*cm).server_address_list );
	}
	if( err!=0 ){
		fprintf( stderr, "\n'%s': Error %i in getaddrinfo, '%s'.", argv[0], err, gai_strerror( err ) );
		exit( err );
	}

	if( (*cm).session_databases<=0 ){
		fprintf( stderr, "\n'%s': No set databases (%i).", argv[0], (*cm).session_databases );
		exit( err );
	}

	/*
	 * MEMC */
	err = memc_init( &(*cm) );
	if( err>=CBERROR ){ cb_clog( CBLOGERR, err, "\nmemc_init, error %i.", err ); }

	err = memc_connect( &(*cm), &key, keylen );
	if( err>=CBERROR ){ cb_clog( CBLOGERR, err, "\nmemc_connect, error %i.", err ); }

	//fprintf( stderr, "\nmain: iplen %i, portlen %i", portlen, iplen );

	switch ( cmd ) {
		case MEMCGET:
			err = memc_get(  &(*cm), &key, keylen, &msg, &msglen, (int) MESSAGELEN, &cas, 0 ); // MAXPATHLEN, &cas, 0 );
			//cb_clog( CBLOGDEBUG, CBNEGATION, "\nmain: memc_get return %i", err );
			//cb_flush_log();
			cb_clog( CBLOGINFO, CBSUCCESS, "\nKey: ");
		        for( i=0; i<keylen; ++i )
				cb_clog( CBLOGINFO, CBSUCCESS, "%c", (unsigned char) key[i] );
			cb_clog( CBLOGINFO, CBSUCCESS, " Value: ");
		        for( i=0; i<msglen; ++i )
				cb_clog( CBLOGINFO, CBSUCCESS, "%c", (unsigned char) msg[i] );
			cb_clog( CBLOGINFO, CBSUCCESS, ".");
			break;
		case MEMCSET:
			err = memc_set( &(*cm), &key, keylen, &msg, msglen, 0, 0, EXPIRATION );
			cb_clog( CBLOGDEBUG, CBNEGATION, "\nmain: memc_set return %i", err );
			cb_flush_log();
			break;
		case MEMCDELETE:
			err = memc_delete( &(*cm), &key, keylen, 0, 0 );
			cb_clog( CBLOGDEBUG, CBNEGATION, "\nmain: memc_delete return %i", err );
			cb_flush_log();
			break;
		/**
0		case SASLLIST:
			err = memc_sasl_list(  &(*cm), &msg, &msglen, (int) MAXPATHLEN, 0 );
			cb_clog( CBLOGDEBUG, CBNEGATION, "\nmain: memc_sasl_list return %i", err );
			cb_flush_log();
			cb_clog( CBLOGINFO, CBSUCCESS, " SASL list: ");
		        for( i=0; i<msglen; ++i )
				cb_clog( CBLOGINFO, CBSUCCESS, "%c", (unsigned char) msg[i] );
			cb_clog( CBLOGINFO, CBSUCCESS, ".");
			break;
		 **/
		case MEMCQUIT:
			err = memc_quit( &(*cm) );
			cb_clog( CBLOGDEBUG, CBNEGATION, "\nmain: memc_quit return %i", err );
			cb_flush_log();
			break;
		default:
			break;
	}
	if( err>=CBERROR ){ 
		cb_clog( CBLOGERR, err, "\n%s: command returned error %i.", argv[0], err ); 
	}

	/*
	 * Wait for the threads at the same time, 7.8.2018. */
	err =  memc_quit( &(*cm) ); // EIKO TASSA PITAISI OLLA JOIN ENSIN JOTEN WAIT_ALL KAHTEEN KERTAAN PITAISI OLLA TURHA, 7.8.2018
	if( err>=CBERROR ){ 
		cb_clog( CBLOGERR, err, "\n%s: memc_quit, error %i.", argv[0], err ); 
	}

	err = memc_wait_all( &(*cm) );
	if( err>=CBERROR ){ 
		cb_clog( CBLOGERR, err, "\n%s: memc_wait_all, error %i.", argv[0], err ); 
	}

	/*
	 * Free. */
	err = memc_free( cm );
	if( err>=CBERROR ){
	  fprintf( stderr, "\nmemc_free, error %i, errno %i '%s'.", err, errno, strerror( errno ) );
	  fflush( stderr );
	  exit( err );
	}
	return CBSUCCESS;
}
int get_ip_and_port(unsigned char **ip, int *iplen, unsigned char **port, int *portlen, char *ipandport[], int len){
        int err = CBSUCCESS;
        int portnum = 0;
        err = get_urlform_ip_and_port( (* (unsigned char **)  ipandport), len, &(* (unsigned char **) ip), &(*iplen), &portnum );
        *portlen = snprintf( &(* (char**) port)[0], (size_t) MEMCPORTLEN, "%d", portnum );
        return err;
}

