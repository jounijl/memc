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

/*
 * URL-form ip-addresses.
 * 
 * Copyright Jouni Laakso 2006, 2011 and 2012
 *
 */
// Success:
#define IPUFSUCCESS 			0
#define IPV4SUCCESS 			1
#define IPV4FORMAT 			IPV4SUCCESS
#define IPV6SUCCESS 			2
#define IPV6FORMAT 			IPV6SUCCESS
// Error:
#define IPUFERROR			10 
#define IPUFERRMALLOC			11
//
#define IPV4ERROR 			20
#define IPV4ERRMALLOC			21
#define IPV4ERRADDRFORMAT		22
//
#define IPV6ERROR 			40
#define IPV6ERRMALLOC			41
#define IPV6ERRADDRFORMAT		42
// Informational:
#define IPUFINFORMATIONAL		60
#define IPUFNOPORT			61
#define IPUFNOADDRESS			62
#define IPUFTEXTNULL 			63
//
#define IPV4INFORMATIONAL		80
#define IPV4NOPORT			81
#define IPV4NOADDRESS			82
#define IPV4TEXTNULL 			83
#define NOTINIPV4URLFORMAT 		84
//
#define IPV6INFORMATIONAL		100
#define IPV6NOPORT			101
#define IPV6NOADDRESS			102
#define IPV6TEXTNULL 			103
#define NOTINIPV6URLFORMAT 		104
//
#define IPV6URLMAXLEN			70
#define IPV4URLMAXLEN			60

//
// http://www.ietf.org/rfc/rfc2732.txt
//
// Allocates address and port from "IPv6 Literal Address in URL" RFC 2732 format length tlen text
//
// Returns port as -1 if it's not found. If address is not found, copies new address length
// IPV6URLMAXLEN from 'text'. Also in url format ipv4 over ipv6 ::ipv4 (or now any other with bracket).
int ipv6_address_and_port(unsigned char *text, int tlen, unsigned char **addrtext, int *alen, int *port);

//
// IPv4 Literal Address in URL (addrtext and port is chopped from the same textarray - not allocated)
int ipv4_address_and_port(unsigned char *text, int tlen, unsigned char **addrtext, int *alen, int *port);

//
// Determines from first bracket IPv4 or IPv6, returns IPV4FORMAT or IPV6FORMAT
int get_urlform_ip_and_port( unsigned char *text, int tlen, unsigned char **addrtext, int *alen, int *port);
//
// Allocates new one byte text array from four byte text array and does not free neither (ucstext can be freed).
int get_urlform_ip_and_port_ucs( unsigned char *ucstext, int ucstlen, unsigned char **addrtext, int *alen, int *port);

//
// Determines if text string is closer to ipv6 or ipv4 address 
// in it's format without brackets. Without port-part :port.
int get_ip_form_type(unsigned char *text, int tlen);

