#!/bin/sh

LIBCBPATH="../run/lib"

#Linux: CC="/usr/bin/clang -std=gnu17 -pthread"
#Linux: LD="/usr/bin/clang -v -std=gnu17 -pthread "
CC="/usr/bin/clang -std=c11"
LD="/usr/bin/clang -v "

SRCS=" main.c memc.c "
OBJS=" memc.o "
FSRCS=" ./ext/get_option.c ./ext/ipvxurlformat.c ./ext/ipvxformat.c "
FOBJS=" ./get_option.o ./ipvxurlformat.o ./ipvxformat.o "
FLAGS=" -O0 -g -Weverything -fPIC -I. -I/usr/include -I../include "
LDFLAGS=" -lc -L/lib -L/usr/lib -I. -I/usr/include -L. -L../lib -L${LIBCBPATH} -lthr "

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
$LD -lcb -L. $LDFLAGS $OBJS main.o $FOBJS -o memc

$LD -shared -Wl -lcb -L. $LDFLAGS -o ./libmemc.so $OBJS

rm memc.a
rm main.o
ar -rcs memc.a $OBJS $FOBJS

