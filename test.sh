LD_LIBRARY_PATH="${LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH

IP=
PORT1=
PORT2=
HOSTIP=

clear

#
# Note 1: the IP may not be the same as the memcached servers
# Note 2: the connecting port (flag -p) may be used only once
#

# Set
echo ; echo -n "*** test SET ***"
time ./memc -r 2 -s -k "KEYKEYKEY" -m "MS2MS2MS2" -i ${HOSTIP} ${IP}:${PORT1} ${IP}:${PORT2}
#time ./memc -r 1 -s -k "KEYKEYKEY" -m "MSGMSGMSG" -i ${HOSTIP} -p 1234 ${IP}:${PORT1} ${IP}:${PORT2}
# Get
echo ; echo ; echo -n "*** test GET ***"
time ./memc -r 2 -g -k "KEYKEYKEY" -i ${HOSTIP} ${IP}:${PORT1} ${IP}:${PORT2}
#time ./memc -r 2 -g -k "KEYKEYKEY" -i ${HOSTIP} -p 1235 ${IP}:${PORT1} ${IP}:${PORT2}
# Delete
echo ; echo ; echo -n "*** test DELETE ***"
time ./memc -r 2 -d -k "KEYKEYKEY" -i ${HOSTIP} ${IP}:${PORT1} ${IP}:${PORT2}
