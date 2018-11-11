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

#include <stdio.h>    // string functions

#define GETOPTSUCCESS              0    // -t <value> ; match and value found
#define GETOPTSUCCESSATTACHED      1    // -t<value>  ; match value found but value is attached in previous flag, test value
#define GETOPTSUCCESSPOSSIBLEVALUE 2    // -xty <value> ; flagline was there somewhere and value was next (some) argument
#define GETOPTSUCCESSNOVALUE       3    // -xty ; match found without value
#define GETOPTNOTFOUND             -1   // no match and no value
#define GETOPTNOTOPTION            -2   // '-' was missing
#define GETOPTERROR                -10  // error occurred


/*
 * Test flag and return its value (for example -x <value>). 
 * Valuepointer must be null, it is substituted by substring of
 * argv1. 
 *
 * Return success if match occurs and value if it exists in argv2.
 * Value can return NULL. argv2 may be NULL if value is not expected.
 */

int get_option( const char *argv1, const char *argv2, char chr, char **value);
