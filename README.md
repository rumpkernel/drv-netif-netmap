netmap-rumptcpip [![Build Status](https://travis-ci.org/anttikantee/netmap-rumptcpip.png?branch=master)](https://travis-ci.org/anttikantee/netmap-rumptcpip)
================

This repository contains a [netmap](http://info.iet.unipi.it/~luigi/netmap/)
backed network interface for [rump
kernels](https://www.netbsd.org/docs/rump/), allowing networking stacks
provided by rump kernels to use netmap for packet shoveling.

Build instructions
------------------

* `git submodule update --init`
* `./buildrump.sh/buildrump.sh -T rumptools -s rumpsrc`
* `(cd libnetmapif ; ../rumptools/rumpmake dependall && ../rumptools/rumpmake install)`

TODO
----

* use linkstr to specify netmap args (instead of RUMP_NETIF)
* update to newer virtif hypercalls
