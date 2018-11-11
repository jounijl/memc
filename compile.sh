#!/bin/sh

LIBCBPATH=./

CC="/usr/bin/cc -std=c11"
LD="/usr/bin/clang -v "

SRCS=" main.c memc.c "
OBJS=" memc.o "
FSRCS=" ./ext/get_option.c ./ext/ipvxurlformat.c ./ext/ipvxformat.c ./ext/cb_endian.c "
FOBJS=" ./get_option.o ./ipvxurlformat.o ./ipvxformat.o ./cb_endian.o "
FLAGS=" -O0 -g -Weverything -I. -I/usr/include -I../include "
LDFLAGS=" -lc -L/lib -L/usr/lib -I. -I/usr/include -L. -L../lib -L${LIBCBPATH} -lcb -lthr "

for OBJ in $OBJS $FOBJS
 do
   rm $OBJ
done

rm main.o

for I in $SRCS $FSRCS
 do
   echo "SRCS: $I"
   $CC $FLAGS -c $I
done


#echo "LINKER: $LD $LDFLAGS $OBJS main.o $FOBJS -o memc"
$LD $LDFLAGS $OBJS main.o $FOBJS -o memc

$LD -shared -Wl $LDFLAGS -o ./libmemc.so $OBJS

rm memc.a
rm main.o
ar -rcs memc.a $OBJS $FOBJS

