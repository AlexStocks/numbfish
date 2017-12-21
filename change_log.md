# Numbfish #
---
 *a service governance library*

## Introdction ##
---
> DESC       : Numbfish is a service goverance library using C/C++ in agent pattern. Numbfish is based on [nlb](https://github.com/Tencent/MSEC/tree/master/nlb) which refers to Tencent's L5. In numbfish there are three work modes: client mode, server mode and mix mode. In client mode, the agent gets service info from zookeeper which supplied to clients on the same host. In server mode, the agent register server cpu/mem/network info and supplied services to zookeeper. If the service run in proxy mode, the numbfish should run in mix mode.
>
> LICENCE    : Apache License 2.0

## Develop history ##
---

- 2017/12/21
    > improvement
    * 1 bool -> BOOL, true -> TRUE, false -> FALSE
    * 2 add network stat;

- 2017/12/19
    > feature
    * 1 init
    * 2 version: 0.0.1
