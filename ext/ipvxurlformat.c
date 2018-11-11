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
#include <string.h> // memset
#include <stdlib.h> // malloc
#include "./ipvxformat.h"

// end of file or linefeed
#define LF( x ) 		( x != 0x0D || x != 0x0A || x != (unsigned char) EOF )

#define PORTTEXTSIZE	10

#define LEFTBRACKET  	'['

int allocate_address_and_port(unsigned char **addr, int alen, int **port);

//
// IPv6 Literal Address in URL's, RFC 2732
// '['IPv6address']'[:[port]]
//
// If port is not found, returns -1.
//
// bugs: returns undefined results if tlen is longer than text length - returns undefined err
// (possibly over/underflowed) and empty values, port -1.
//
int ipv6_address_and_port(unsigned char *text, int tlen, unsigned char **addr_text, int *alen, int *port){
	int i1=0,i2=0,i3=0,i4=0,cptr=0,addrlen=0,err=IPV6SUCCESS;
	unsigned char port_text[PORTTEXTSIZE+1];
	unsigned char addr[IPV6URLMAXLEN+1];
	memset( &addr, (int) '0',  IPV6URLMAXLEN);
	memset( &port_text, (int) '0',  PORTTEXTSIZE);
	addr[IPV6URLMAXLEN]='\0';
	port_text[PORTTEXTSIZE]='\0';

	if(text==NULL)
	  return IPV6ERROR;

	if( alen==NULL || port==NULL || addr_text==NULL ){		
		return IPUFERRMALLOC;
	}

	for(i1=0;(i1<IPV6URLMAXLEN&&i1<tlen&&text[i1]!=(unsigned char)EOF);++i1){ // [
	  if(text[i1]=='['){
	    ++i1; cptr+=i1;
	    for(i2=0;((cptr+i2)<IPV6URLMAXLEN&&(cptr+i2)<tlen&&LF(text[cptr+i2]));++i2){ // ]
 	      if(text[i1+i2]==']'){
	        addr[i2]='\0'; ++i2; cptr+=i2; addrlen=i2;
		for(i3=0;((cptr+i3)<IPV6URLMAXLEN&&(cptr+i3)<tlen&&LF(text[cptr+i3]));++i3){ // : '\n'
	          if(text[cptr+i3]==':'){
	            ++i3; cptr+=i3;
	            for(i4=0;((cptr+i4)<IPV6URLMAXLEN&&(cptr+i4)<tlen&&LF(text[cptr+i4]));++i4){ // EOF '\n'
	              port_text[i4]=text[cptr+i4];
	            }
	            port_text[i4]='\0'; // if i4==0 port is zero length
	            i3=IPV6URLMAXLEN+1;
	          }
	        }
	        i2=IPV6URLMAXLEN+1;
	      }else{
	        addr[i2]=text[cptr+i2];
 	      }
	    }
	    i1=IPV6URLMAXLEN+1;
	  }
	}

	//
	// Memory allocation of return values
	if(i1==IPV6URLMAXLEN || (i3==0 && i1>=IPV6URLMAXLEN)){ // not found or only [ // 3
	  addrlen=IPV6URLMAXLEN;
	  err=IPV6ERRADDRFORMAT;
	}
	err = allocate_address_and_port( &(*addr_text), addrlen, &port );
	if(err==IPV6ERRMALLOC){
	  perror("error in allocating address ");
	  return err;
	}

	//*alen = (int) malloc( sizeof(int) );
	//if(alen==NULL){ return IPV6ERRMALLOC; }
	//*port = (int) malloc( sizeof(int) );
	//if(port==NULL){ return IPV6ERRMALLOC; }

	// 
	// Copy values
	*alen = addrlen; // 1
	//19.7.2017: if( port_text!=NULL && i4!=0 ){ // 2
	if( i4!=0 ){ // 2
	  *port = (int) strtol(&(* (char*) port_text), (char **)NULL, 10);
	  if(*port==0) // strtol, errno EINVAL
	    err = IPV6NOPORT;
	}else if( i4==0 ){
	  *port = -1;
	  err = IPV6NOPORT;
	}else{
	  port=NULL;
	  err = IPV6ERRMALLOC;
	}
	if(i1==IPV6URLMAXLEN || (i3==0 && i1>=IPV6URLMAXLEN) ){ // 3
	  memcpy(&(**addr_text),&(*text),IPV6URLMAXLEN);
	  err=NOTINIPV6URLFORMAT;
	}else{
	  //memcpy(&(**addr_text),&(*addr),addrlen);
	  if(addrlen>=0)
	    memcpy(&(**addr_text),&(*addr), (unsigned int) addrlen);
	}
	return err;
}

int allocate_address_and_port(unsigned char **addr, int alen, int **port){
	if( alen<0 || addr==NULL ) // 4.2.2015
	  return IPUFERRMALLOC;
	if( addr==NULL || port==NULL ) return IPUFERRMALLOC; // 29.10.2016
	*addr = (unsigned char*) malloc( ( (unsigned int) alen+1)*sizeof(char) );
	if(*addr==NULL){ return IPUFERRMALLOC; }
	(*addr)[alen]='\0';
	//if(*port!=NULL)
	if(port!=NULL) // 4.1.2015
	 *port = (int *) malloc( sizeof(int) );
	if( port==NULL || *port==NULL ){ return IPUFERRMALLOC; }
	return IPUFSUCCESS;
}

int get_urlform_ip_and_port_ucs( unsigned char *ucstext, int ucstlen, unsigned char **addrtext, int *alen, int *port){
	unsigned int indx=0, undx=0;
	unsigned char *onebytetext = NULL;
	int onebytetextlength = 0;
	if( ucstext==NULL || addrtext==NULL || alen==NULL || port==NULL )
	  return IPUFTEXTNULL;
	onebytetextlength = ucstlen/4;
	onebytetext = (unsigned char*) malloc( ( (unsigned int) onebytetextlength + 1 )*sizeof(unsigned char) );
	if( onebytetext==NULL )
	  return IPUFERRMALLOC;
	memset( &onebytetext[0], 0x20, (size_t) onebytetextlength );
	onebytetext[onebytetextlength] = '\0';
	for( indx=0; indx < (unsigned int) onebytetextlength && (indx*4) < (unsigned int) ucstlen; ++indx){
	  undx=(indx+1)*4-1;
	  onebytetext[indx] = ucstext[undx];
	}
	if(indx<(unsigned int)onebytetextlength)
	  onebytetext[indx+1] = '\0'; 
	return get_urlform_ip_and_port( &onebytetext[0], onebytetextlength, &(*addrtext), &(*alen), &(*port) );	
}
int get_urlform_ip_and_port( unsigned char *text, int tlen, unsigned char **addrtext, int *alen, int *port){
	int err=IPUFSUCCESS;
	if( addrtext==NULL || alen==NULL || port==NULL ) return IPUFERRMALLOC;
	if( text != NULL ){
	  if(text[0]==LEFTBRACKET){
	    err = ipv6_address_and_port(&(*text),tlen,&(*addrtext),&(*alen),&(*port));
	    if(err<=IPUFERROR || err >= IPUFINFORMATIONAL)
	      return IPV6FORMAT;
	  }else{
	    err = ipv4_address_and_port(&(*text),tlen,&(*addrtext),&(*alen),&(*port));
	    if(err<=IPUFERROR || err >= IPUFINFORMATIONAL)
	      return IPV4FORMAT;
	  }
	}else
	    return IPUFTEXTNULL;
	return err;
}

int ipv4_address_and_port(unsigned char *text, int tlen, unsigned char **ip, int *iplen, int *port){
        unsigned char *p;
        unsigned char temp=1;
        int indx=0, err=IPV4SUCCESS;

	//19.7.2017, strtok: if( ip==NULL || *ip==NULL || iplen==NULL || port==NULL ) return IPUFERRMALLOC;
	if( ip==NULL || iplen==NULL || port==NULL ) return IPUFERRMALLOC;

        p = &temp; // not null
        if(text==NULL) // replace these with strtok with malloced ip and tlen loop
          return IPV4TEXTNULL;
        for(indx=0;indx<tlen;++indx)
          if(text[indx]=='\0')
            return IPV4TEXTNULL; // /these

        if( text!=NULL && text[0]!=':'){
           *ip = (unsigned char*) strtok(&(* (char*) text),":"); // string ":", left side
           if ( *ip != NULL )
             p = (unsigned char*) strtok(NULL,":"); // "When no more tokens remain, a null pointer is returned.", right side
          else
             err = IPV4NOADDRESS;
        }else{
          err = IPV4NOADDRESS;
          p = (unsigned char*) strtok(&(* (char*) text),":"); // string ":", right side
        }          

        if(p!=NULL){
          *port = (int) strtol( &(* (char *) p), (char **)NULL, 10); // If value cannot be represented, behaviour is undefined, man atoi
        }else{
          *port=-1;
          if(err!=IPV4SUCCESS){
            perror("ipv4_address_and_port, no tokens \':\'. Both ip and port is missing.");
            err = NOTINIPV4URLFORMAT; // both port and ip is missing
        }else
            err = IPV4NOPORT;
        }
        if(ip==NULL || *ip==NULL)
          *iplen = 0;
        for(indx=0;indx<IPV4URLMAXLEN && ip!=NULL && *ip!=NULL; ++indx){
          if( (*ip)[indx]=='\0'){
             //*iplen = indx-1; indx = IPV4URLMAXLEN;
             *iplen = indx; indx = IPV4URLMAXLEN;
          } 
        }
        return err;
}

