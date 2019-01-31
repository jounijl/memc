/*
 *
 * Copyright (c) 2015 and 2016 Jouni Laakso
 *
 */

typedef struct db_conn_param {

	// Two variables in memc
        unsigned char     *ip;
        unsigned char     *port;
        int                iplen;
        int                portlen;
	// /Two variables in memc

} db_conn_param;

