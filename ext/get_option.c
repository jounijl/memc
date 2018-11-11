/*
 * Copyright (c) 2006, 2011 and 2012, Jouni Laakso
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
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

#include <errno.h>
#include <stdio.h>
#include <string.h> 

#include "./get_option.h"

/*
 * Test flag and return its value. Valuepointer must be null. It is 
 * substituted by substring of argv1.
 *
 * Return success if match occurs and value if it exists in argv2.
 * Value can return NULL. argv2 may be NULL if value is not expected.
 */

int get_option(const char *argv1, const char *argv2, char chr, char **value){
        int  err = 0; char *chr_ptr = NULL;
        char test_str[4] = { '-','0',' ','\0' };

        if(argv1==NULL){
	  perror("\nget_option: argv1 was null, returning error.");
          return GETOPTERROR;
        }

        err = snprintf( &(*test_str), (size_t) 3, "-%c ", chr);
        if(err!=3){ perror("\nget_option:"); fprintf(stderr," %i", errno); return GETOPTERROR; }

        // First is line
        if( strncmp(argv1,"-",1)!=0 ){
	  //perror("\nget_option: '-' was missing, returning error.");
          return GETOPTNOTOPTION;
        }

        // Values with parameter
        err = strncmp(argv1, test_str, (size_t) 3); // "-'chr' 'value'" ; -t xyz or -t <value>
        if(err==0){ // match
          if(argv2!=NULL)
            *value = (char *) &(*argv2);
	  else
	    *value = NULL;
          return GETOPTSUCCESS;
        }

        // Values without parameter
        err = strncmp(argv1, test_str, (size_t) 2); // "-'chr''value'" ; -txyz or -t<value>
	if(err==0){ // match
          // parse value and return pointer
          if( ( chr_ptr = strchr(argv1, (int) chr) ) != NULL )
            *value = &( chr_ptr[1] );
          return GETOPTSUCCESSATTACHED;
        }

        // Values without parameter
        if ( argv1[0]=='-' ){
          if( strchr(argv1, (int) chr) != NULL ){ // "-"[a-z]+'chr'[a-z]+
            if(argv2!=NULL){
              *value = (char *) &(*argv2);
              return GETOPTSUCCESSPOSSIBLEVALUE;
            }else{
              *value = NULL;
              return GETOPTSUCCESSNOVALUE;
            }
          }
        }

        return GETOPTNOTFOUND;
}

