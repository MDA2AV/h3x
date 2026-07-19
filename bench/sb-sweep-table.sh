#!/usr/bin/env bash
# Median table from a sb-sweep log. Usage: bench/sb-sweep-table.sh [bench/sb-sweep.log]
awk '
/^RESULT/ {
  split($3,a,"="); split($4,b,"=");
  key=$2" "a[2]" "b[2]; n[key]++; i=n[key];
  r[key","i]=$6; f[key","i]=$7;
  split($8,l,"="); split($9,s,"=");
  lg[key","i]=l[2]; sv[key","i]=s[2];
}
function m3(A,k, x,y,z,t){ x=A[k",1"]; y=A[k",2"]; z=A[k",3"]; if(x>y){t=x;x=y;y=t} if(y>z){t=y;y=z;z=t} if(x>y){t=x;x=y;y=t} return y }
function rng(k, x,y,z,lo,hi){ x=r[k",1"]; y=r[k",2"]; z=r[k",3"]; lo=x;hi=x; if(y<lo)lo=y; if(y>hi)hi=y; if(z<lo)lo=z; if(z>hi)hi=z; return hi-lo }
END {
  ns=split("h2o nginx haproxy caddy envoy",S," ");
  printf "%-8s %-5s | %8s %8s %6s | %5s %5s | %5s %5s | %5s %5s | %8s %8s\n","server","conns","med_b1","med_b8","delta","f1","f8","lg1","lg8","srv1","srv8","rng_b1","rng_b8";
  for(i=1;i<=ns;i++) for(c=512;c<=1024;c+=512){
    k1=S[i]" "c" 1"; k8=S[i]" "c" 8";
    if(n[k1]=="" || n[k8]=="") continue;
    printf "%-8s %-5d | %8d %8d %+5.1f%% | %5d %5d | %5.1f %5.1f | %5.1f %5.1f | %8d %8d\n", S[i],c, m3(r,k1),m3(r,k8),(m3(r,k8)/m3(r,k1)-1)*100, m3(f,k1),m3(f,k8), m3(lg,k1),m3(lg,k8), m3(sv,k1),m3(sv,k8), rng(k1),rng(k8);
  }
}' "${1:-bench/sb-sweep.log}"
