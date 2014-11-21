# monitr

Nodejs process monitoring tool. 

This module currently works only on Linux operating systems.
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
Sets the datagram socket name to write the stats. Defaults to /tmp/nodejs.mon

# Health Status
Monitr now supports custom health functionality whereby the app can report its own health.
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

Due to the method by which the stack backtrace is
obtained (installing a v8 Javascript engine debug event listener and
simulating a debugger event), there _is_ a performance slowdown for
code running while the stack backtrace option is active.

By default the backtrace option is not enabled.  You can enable it
setting the `showBacktrace` property to true, e.g.

```js
monitor.showBacktrace = true
```

Setting `monitor.showBacktrace` to `false` will restore the original
performance by removing the debug event listener.

# Example

Please refer to the examples/README.md for examples showing the use of these functions.

# Build Status

[![Build Status](https://secure.travis-ci.org/yahoo/monitr.png?branch=master)](http://travis-ci.org/yahoo/monitr)

# Node Badge

[![NPM](https://nodei.co/npm/monitr.png)](https://nodei.co/npm/monitr/)

