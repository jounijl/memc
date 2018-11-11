/*
 *
 * Copyright (c) 2015 and 2016 Jouni Laakso
 *
 */

typedef struct db_conn_param {

        /*
	 * These three variables are used with session database. */
        unsigned char     *modulename;    // module name in rvp.conf
        unsigned char     *ip;
        unsigned char     *port;
        int                modulenamelen;
        int                iplen;
        int                portlen;
        /* /Three variables. */

	// Write to this (pointer is copied to transaction_params)
	unsigned int       connection_timeout; // 
        void              *dbconn;             // Cast to any, 14.9.2016 ( In PostgreSQL: PGconn *dbconn )

        unsigned char     *encoding;           // transaction encoding
	unsigned char     *fprefix;
        int                encodinglen;
	int                fprefixlen;

        unsigned char     *username;
        unsigned char     *password;
        unsigned char     *dbname;
        int                usernamelen;
        int                passwordlen;
        int                dbnamelen;

	/* Number of exec calls made, 4.4.2017. */
	int                exec_count;

} db_conn_param;

