#!/usr/bin/env python3
"""UDP drop accounting for the benchmark grid.

Two facts drove this. First, every drop we have seen on this box is a receive-buffer overflow:
across matched runs the per-socket drop counter, snmp RcvbufErrors and snmp InErrors were exactly
equal, so a single number per side is enough and we do not need to break errors down by kind.
Second, server-side and client-side drops are wildly different things and must not be summed: h2o
dropped 183k datagrams in a 5s run while completing every request (QUIC just retransmits), whereas
haproxy dropped zero datagrams and still failed ~1000 requests. Throughput tables and reliability
tables are answering different questions.

Server drops are read per socket, matched by local port, so they are exact. Client drops are
derived: global RcvbufErrors minus the server's share. That is necessary rather than lazy, because
the client's sockets are created and destroyed inside the run and their per-socket counters vanish
with them, while the global counter persists. It assumes no other meaningful UDP traffic on the box
during a cell, which holds for a loopback benchmark but would not on a shared machine.

  udpstat.py snap <file>                    write a snapshot
  udpstat.py delta <before> <after> <port>  print "srvdrops=N clidrops=N totdrops=N indgrams=N"
"""
import json
import sys


def _sockets():
    """port -> summed drop counter, over both IPv4 and IPv6 UDP tables."""
    out = {}
    for path in ("/proc/net/udp", "/proc/net/udp6"):
        try:
            lines = open(path).read().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            f = line.split()
            if len(f) < 13:
                continue
            try:
                port = int(f[1].split(":")[1], 16)
                drops = int(f[-1])
            except ValueError:
                continue
            out[port] = out.get(port, 0) + drops
    return out


def _snmp():
    lines = open("/proc/net/snmp").read().splitlines()
    for i, line in enumerate(lines):
        if line.startswith("Udp:") and "InDatagrams" in line:
            keys = line.split()[1:]
            vals = [int(x) for x in lines[i + 1].split()[1:]]
            return dict(zip(keys, vals))
    return {}


def snap():
    return {"sock": _sockets(), "snmp": _snmp()}


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "snap":
        with open(sys.argv[2], "w") as f:
            json.dump(snap(), f)
        return 0
    if len(sys.argv) >= 5 and sys.argv[1] == "delta":
        try:
            before = json.load(open(sys.argv[2]))
            after = json.load(open(sys.argv[3]))
        except (OSError, ValueError):
            print("srvdrops=-1 clidrops=-1 totdrops=-1 indgrams=-1")
            return 0
        port = sys.argv[4]
        srv = after["sock"].get(port, 0) - before["sock"].get(port, 0)
        tot = after["snmp"].get("RcvbufErrors", 0) - before["snmp"].get("RcvbufErrors", 0)
        ind = after["snmp"].get("InDatagrams", 0) - before["snmp"].get("InDatagrams", 0)
        # Counters are cumulative and only ever rise; a negative delta means the socket was recreated
        # mid-cell (a server restart), so report -1 rather than a bogus small number.
        if srv < 0 or tot < 0:
            print("srvdrops=-1 clidrops=-1 totdrops=-1 indgrams=-1")
            return 0
        cli = tot - srv
        if cli < 0:  # server counted more than the global total: sampling skew, clamp not invent
            cli = 0
        print(f"srvdrops={srv} clidrops={cli} totdrops={tot} indgrams={ind}")
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
