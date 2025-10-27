#!/usr/bin/env bash


lses=`ls -l | awk '{print $9}' | tr '-' ' ' | awk '{print $2}' | sort | uniq | sort -n`

#rpses
rm -f rpses
rm -f backends
rm -f frontends
rm -f speculateds
rm -f retirings*
for ts in $lses;
do
    echo -n "$ts " >> rpses;cat result.1-$ts | fgrep ttfb | cut -f 3 -d ' ' >> rpses;
    echo -n "$ts " >> backends;cat pstat$ts | fgrep backend | awk '{print $3}' >> backends;
    echo -n "$ts " >> frontends;cat pstat$ts | fgrep front | awk '{print $2}' >> frontends;
    echo -n "$ts " >> speculateds;cat pstat$ts | fgrep specul | awk '{print $2}' >> speculateds;
    echo -n "$ts " >> retirings;cat pstat$ts | fgrep retir | awk '{print $2}' >> retirings;

done
#ttfbs
rm -f ttfbs
for ts in $lses;
do
    echo -n "$ts " >> ttfbs;cat result.1-$ts | fgrep ttfb | cut -d ' ' -f 22 >> ttfbs;
    echo -n "$ts " >> ttfbs;cat result.1-$ts | fgrep ttfb | cut -d ' ' -f 22 >> ttfbs;
done
