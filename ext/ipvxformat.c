/* 
 * Copyright (C) 2006, 2011 and 2012. Jouni Laakso
 * 
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General
 * Public License version 2.1 as published by the Free Software Foundation 6. of June year 2012;
 * 
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details. You should have received a copy of the GNU Lesser General Public License along with this library; if
 * not, write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * licence text is in file LIBRARY_LICENCE.TXT with a copyright notice of the licence text.
 */

#include <stdio.h>
#include "./ipvxformat.h"

// end of file or linefeed
#define LF( x ) 		( x != 0x0D || x != 0x0A || x != (unsigned char) EOF )

//
// Determines from ipv4 and ipv6 forms witch address is text:s address.
// Now: from first colon.
int get_ip_form_type(unsigned char *text, int tlen){
	int indx=0; unsigned char chr='0'; int ret=IPV4FORMAT;
	if(text==NULL)
	  return IPUFTEXTNULL;
	for(indx=0;indx<tlen && chr!='\0' && ! LF( chr );++indx){
	  chr=text[indx];
	  if( chr == ':' ){ // ipv6 contains colon, ipv4 doesn't
	    ret=IPV6FORMAT;
	    indx=tlen;
	  }
	}
	return ret;
}
