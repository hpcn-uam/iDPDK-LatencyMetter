#!/bin/bash

# 0000:19:00.0 enp25s0f0
# 0000:19:00.1 enp25s0f0
# 0000:d8:00.0 enp216s0f0
# 0000:d8:00.1 enp216s0f1

/home/naudit/iDPDK-LatencyMetter/dpdk/usertools/dpdk-devbind.py -b ixgbe 0000:d8:00.0

#ifconfig enp216s0f0 10.120.120.122/24 up promisc
ifconfig enp216s0f0 10.1.2.216/24 up promisc hw ether 0:1B:21:D8:0:0