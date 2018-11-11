/* 
 * Library to read and write streams. Different character encodings.       
 * 
 * Copyright (C) 2009, 2010, 2013, 2014, 2015 and 2016. Jouni Laakso
 *
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * Otherwice, this library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser        
 * General Public License version 2.1 as published by the Free Software Foundation 6. of June year 2012;
 * 
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details. You should have received a copy of the GNU Lesser General Public License along with this library; if
 * not, write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * 
 * licence text is in file LIBRARY_LICENCE.TXT with a copyright notice of the licence text.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
//removed 20180822 #include "../include/cb_endian.h"
#include "../include/cb_buffer.h"

/* 
 * Reverse 32 bits.
 */
unsigned int  cb_reverse_int32_bits(unsigned int from){
	unsigned int upper=0, lower=0, new=0;
	upper = from>>16; // 16 upper bits
	lower = 0xFFFF & from; // 16 lower bits
        lower = cb_reverse_int16_bits(lower);
	upper = cb_reverse_int16_bits(upper);
	new = lower<<16;
	new = new | upper;
	return new;
}
/* 
 * Reverse integers last 16 bits. First 16 bits are meaningless.
 */
unsigned int  cb_reverse_int16_bits(unsigned int from){
	unsigned int  new=0;
	unsigned char upper=0, lower=0;
	upper = (unsigned char) from>>8; // 8 upper bits
	lower = 0xFF & from; // 8 lower bits
        lower = cb_reverse_char8_bits(lower);
	upper = cb_reverse_char8_bits(upper);
	new = new&lower; // 6.12.2014
	new = new<<8; // 6.12.2014
	//new =  lower<<8;

	new = new | upper;
	return new;
}
/*
 * Reverse 8 bits.
 * 1. 2. 3. 4. 5. 6. 7. 8.
 * 1  2  4  8  16 32 64 128
 */
unsigned char  cb_reverse_char8_bits(unsigned char from){
	unsigned int new = 0x0000, ifrom = (unsigned int) from;
	new = new | (ifrom & 0x01)<<7;
	new = new | (ifrom & 0x02)<<5;
	new = new | (ifrom & 0x04)<<3;
	new = new | (ifrom & 0x08)<<1;

	new = new | (ifrom & 0x10)>>1; // 16
	new = new | (ifrom & 0x20)>>3; // 32
	new = new | (ifrom & 0x40)>>5; // 64
	new = new | (ifrom & 0x80)>>7; // 128
	return (unsigned char) new;
}
unsigned int  cb_reverse_four_bytes(unsigned int  from){
	unsigned int upper=0, lower=0, new=0;
	//cb_clog( CBLOGDEBUG, CBNEGATION, "\ncb_reverse_four_bytes: from 0x%.4x to: ", from );
	upper = from>>16; // 16 upper bits
	//cb_clog( CBLOGDEBUG, CBNEGATION, " 0x%.4x ", upper );
	lower = 0xFFFF & from; // 16 lower bits
	//cb_clog( CBLOGDEBUG, CBNEGATION, " 0x%.4x ", lower );
        lower = cb_reverse_two_bytes(lower);
	upper = cb_reverse_two_bytes(upper);
	new = lower; new = new<<16;
	new = new | upper;
	//cb_clog( CBLOGDEBUG, CBNEGATION, "\ncb_reverse_four_bytes: return 0x%.4x.", new );
	return new;
}
unsigned int  cb_reverse_two_bytes(unsigned int  from){
	unsigned int  new=0;
	unsigned char upper=0, lower=0;
	//cb_clog( CBLOGDEBUG, CBNEGATION, "\ncb_reverse_two_bytes: from 0x%.4x to: ", from );
	upper = (unsigned char) ( from >> 8 ); // 8 upper bits
	//cb_clog( CBLOGDEBUG, CBNEGATION, " 0x%.4x ", upper );
	lower = (unsigned char) ( 0xFF & from ); // 8 lower bits
	//cb_clog( CBLOGDEBUG, CBNEGATION, " 0x%.4x ", lower );
	new = lower; new = new<<8;
	new = new | upper;
	//cb_clog( CBLOGDEBUG, CBNEGATION, "\ncb_reverse_two_bytes: return 0x%.4x.", new );
	return new;
}

/*
 * OSI Representation layer:  Host byte order conversion if needed 
 * ----------------------------------------------------------------------
 * OSI Session layer:         4-byte UCS in application or in stored form
 */
unsigned int  cb_from_host_byte_order_to_ucs( unsigned int chr ){
        return cb_from_ucs_to_host_byte_order( chr );
}
unsigned int  cb_from_ucs_to_host_byte_order( unsigned int chr ){
	int test = cb_test_cpu_endianness();
        if( test == CBBIGENDIAN ){ 
	  //cb_clog( CBLOGDEBUG, CBNEGATION, "\ncb_from_ucs_to_host_byte_order: cb_test_cpu_endianness, big endian." );
          return chr;
        }else if( test == CBLITTLEENDIAN ){ 
          return cb_reverse_four_bytes( chr );
        }
        return chr;
}
int  cb_test_cpu_endianness(){
        static union {
          unsigned short  two;
          unsigned char   td[ sizeof(unsigned short) ];
        } hend;
        hend.two = 0xFEFF;
        if( hend.td[0] == 0xFE ){
          return CBBIGENDIAN;
        }else if( hend.td[0] == 0xFF ){
          return CBLITTLEENDIAN;
        }
	return CBUNKNOWNENDIANNESS;
}

