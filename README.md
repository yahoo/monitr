# monitr

Nodejs process monitoring module

This package is tested only with Node.js LTS versions.

_Note: This module currently works only on Linux operating systems_.

## External statistics reporting

This module starts a separate thread within the Nodejs runtime that
monitors and collects statistics about the running nodejs process.
These statistics are then sent as JSON messages via UDP datagrams over
a local domain socket.

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
       jiffyperreq: <cpu usage in terms of ticks per request>,
       gc: {
           scavenge: { count: <number>, elapsed_ms: <number>, max_ms: <number> },
           marksweep: { count: <number>, elapsed_ms: <number>, max_ms: <number> }
       }
    }
 }
```

## GC introspection

It provides the running nodejs application with the ability to
introspect garbage collection activity by creating read-only
properties at `process.monitor.gc` that reports:

1.  `count`: number of times GC stop-the-world events occurred
2.  `elapsed`: cumulative time (in milliseconds) spent in GC

# Installation

With [npm](http://npmjs.org) do:

```
npm install monitr
```

# Usage
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
Sets the datagram socket name to write the stats. Defaults to /tmp/nodejs.mon

# Health Status
Monitr supports custom health functionality whereby the app can report its own health.
The following methods are added to process.monitor to set and get the health information.
```js
setHealthStatus(isDown, statusCode)
isDown()
getStatusCode()
getStatusTimestamp() - Return seconds when setHealthStatus was last called
getStatusDate() - Return Date object
```
Once setHealthStatus is invoked, the status json, described above, will have following additional fields.
```js
health_status_timestamp: <timestamp when the setHealthStatus was invoked, in sec>,
health_is_down: <app is down or up, boolean>,
health_status_code: <health status code>
```

# Handling HUP events

`Monitr` installs a custom `SIGHUP` handler which will optionally
print out a NodeJS stack backtrace of the Javascript currently being
executed.  This can be useful for debugging where a NodeJS process may
be _stuck_.

# Implementation

It looks up /proc/* files on the system to report CPU Usage.  It looks
up /proc/pid/* files on the system to report its own stats.
`process.monitor.*` methods are set by `lib/monitor.js`.

It calls the process.monitor.* methods to report total requests since
monitoring started (`reqstotal`), current requests in flight
(`oreqs`), current open connections (`oconns`) and total data returned
since monitoring started (`kb_trans`).  _Note: `oreqs` may be greater
than `oconns` when keepalive is enabled_.

It attaches to the v8 garbage collection hooks to instrument (for each
GC type) the following stats for each reporting interval.

1.  `count` : number of times GC type invoked
2.  `elapsed_ms`: total elapsed time nodejs thread is blocked
3.  `max_ms`:  maximum time spent blocked by any one GC event

# Example

Please refer to examples/README.md for examples showing the use of these functions.

# Build Status

[![Build Status](https://secure.travis-ci.org/yahoo/monitr.png?branch=master)](http://travis-ci.org/yahoo/monitr)

# Node Badge

[![NPM](https://nodei.co/npm/monitr.png)](https://nodei.co/npm/monitr/)

