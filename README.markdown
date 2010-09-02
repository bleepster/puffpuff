Background
==========
At work, I needed a network test tool that had the following attributes
* generate TCP/UDP traffic on top of IPv4/IPv6
* size of data payload and the frequency in which they are sent are configurable
  on startup but will not change during execution
* duration upon which traffic is sent is configurable
* number of thread instances is configurable

Dependencies
============
* pthread
* libevent

Platform
========
Known to run in the following OSes
* Ubuntu
* FreeBSD

Usage (client)
===============
Required parameters
        [-4 server=<IPv4 address>,client=<IPv4 address> | -6 server=<IPv6 address>,client=<IPv6 address>]
        [-p <port number>]
        [-t <transport protocol (tcp|udp)>]
Optional parameters
        [-S <size of data>]
        [-i <number of thread instances>]
        [-d <delay time>]
        [-T <duration (in seconds)>]
        [-b <run in background>]
Defaults
        Delay: 1000000 usec
        Number of instances: 1
        Process will run in the foreground

Usage (server)
===============
Required parameters
        [-4 <IPv4 address> | -6 <IPv6 address>]
        [-p <port number>]
        [-t <protocol (tcp|udp)>]
Optional parameters
        [-S <size of data>]
        [-i <interval (seconds) for displaying stats>]
        [-d <run as daemon>]
        [-m <maximum allowable connections>]
Defaults
        Size of receive data: 256 bytes
        Display status interval: 1 second
        Maximum allowable sessions: 100
