#!/usr/bin/env bash

PATH=$PATH:/home/dagge/.cargo/bin/
stride="${STRIDE:=64}"

STATPARM="-a -d "
#-a -M tma_backend_bound_group 

# nc determines number of clients and threads in cache
nc=(2 8 16)
# touch size, how much of passed data is touched by the receiver
ts=(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 25 30 35 40 45 50 60 70 80 90 100) # 262144) # 524288 1048576)
for n in "${nc[@]}";
do
  # spawn the cache
  ./cache -t $n &
  sleep 2
  cpid=$!
  ./bridge -t $n &
  sleep 2
  bpid=$!
  
  rm -rf resultsfd$n;
  mkdir resultsfd$n;
  rm -rf resultscopy$n;
  mkdir resultscopy$n;

  for s in "${ts[@]}";
  do
    # figure out the number of requests to run
    #fdruns=`expr 1048576 / $ts`
    fdruns=100000
    copyruns=100000
    if [ $s -lt 128000 ];
    then
      df=`expr $s / 10`
      if [ $df -lt 100 ];
      then
        df=100
      fi
      factor=`expr 10485 / $df`
      factor=`expr $factor / 100`
      fdruns=`expr $fdruns \* $factor`
      if [ $fdruns -lt $copyruns ];
      then
        fdruns=$copyruns
      fi

    fi
    

   
    t=1;while [ $t -le $n ] ;
    do
      echo fd $n $t $fdruns;
      if [ $t -eq $n ];
      then
        #flamegraph --cmd "record -F 99 -a -g" -o resultsfd$n/flame$n-$s-$t -- ./client $RIDX $WRP -s $stride -c $fdruns -t $s > resultsfd$n/result.$t-$s ;
        # perf stat $STATPARM -o resultsfd$n/pstat$s 
        ./client $RIDX $WRP -s $stride -c $fdruns -t $s > resultsfd$n/result.$t-$s ;
      else
        ./client $RIDX $WRP -s $stride -c $fdruns -t $s > resultsfd$n/result.$t-$s &
      fi
      t=`expr $t + 1`;
    done

    #perf record -F 99 -a -g -- sleep 20 &
    
    t=1;while [ $t -le $n ] ;
    do
      echo copy $n $t $copyruns;
      if [ $t -eq $n ];
      then
        #flamegraph --cmd "record -F 99 -a -g" -o resultscopy$n/flamenl$n-$s-$t -- ./client $RIDX $WRP -s $stride -C -c $copyruns -t $s > resultscopy$n/result.$t-$s ;
        #perf stat $STATPARM -o resultscopy$n/pstat$s
        ./client $RIDX $WRP -s $stride -S -c $copyruns -t $s > resultscopy$n/result.$t-$s ;
      else
        ./client $RIDX $WRP -s $stride -S -c $copyruns -t $s > resultscopy$n/result.$t-$s &
      fi
      t=`expr $t + 1`;
    done

  done
  kill $bpid
  # kill the cache
  kill $cpid
done
