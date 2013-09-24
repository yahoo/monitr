# monitr

Nodejs process monitoring tool. This module currently works only on LINUX.
This module spawns a thread and begins monitoring the process. 

It looks up /proc/* files on the system to report CPU Usage.
It looks up /proc/pid/* files on the system to report its own stats.
It calls the process.monitor.* methods to report total requests, open connections and total data transferred.

process.monitor.* methods are set by lib/monitor.js.

Here is the list of data the module reports periodically:
```
 { status: 
     { pid: <pid of the node process>,
       ts: <current time stamp>,
       cluster: <process group id>,
       reqstotal: <total requests processed by this node process server>,
       utcstart: <when the process was started>,
       events: <number of new reports being processed since last stats reporting>,,
       cpu: <cpu usage>,
       mem: <memory usage>,
       cpuperreq: <cpu usage per request>,
       oreqs: <current open requests count>,
       sys_cpu: <system cpu load>,
       oconns: <current open connections count>,
       user_cpu: <user cpu load>,
       rps: <requests per second>,
       kbs_out: <kbs of data transferred since last stats reporting>,
       elapsed: <time elapsed since last event>,
       kb_trans: <total kbs of data transferred>,
       jiffyperreq: <cpu usage in terms of ticks per request> 
    }
 }
```

This package is tested only with Node versions 8 and 10.


# install

With [npm](http://npmjs.org) do:

```
npm install monitr
```

# methods
```js
var monitor = require('monitr');
```

## start()

```js
monitor.start();
```
Spawns a thread and monitors the process. Writes process stats every second to the socket path.

## stop()
```js
monitor.stop();
```
Terminates the thread and closes the socket.

## setIpcMonitorPath(socketPath)
```js
monitor.setIpcMonitorPath('/tmp/my-process-stats.mon');
```
Sets the handle to write the stats to. If not specified, defaults to /tmp/nodejs.mon

# example

Please refer to the examples/README.md for details

# Build Status

[![Build Status](https://secure.travis-ci.org/yahoo/monitr.png?branch=master)](http://travis-ci.org/yahoo/monitr)

# Node Badge

[![NPM](https://nodei.co/npm/monitr.png)](https://nodei.co/npm/monitr/)

