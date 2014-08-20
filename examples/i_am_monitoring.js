/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */
"use strict";
var dgram = require('unix-dgram'),
    fs = require('fs'),
    util = require('util');

/*
 * This is the default path
 * If you plan to edit the path, do so in both monitor_me.js and this file
 * So that they read from the same socket
 */
var monPath = "/tmp/nodejs.mon";
var monitorSocket = dgram.createSocket('unix_dgram');

var last_signal_time = 0;
function checkHungProcess(st) {
    var MAX_INACTIVITY = 5;
    if (st.elapsed > MAX_INACTIVITY * 1000 && (Date.now() - last_signal_time) > MAX_INACTIVITY * 1000) {
	console.error("NodeJS process eventloop not processed for", MAX_INACTIVITY, "seconds.  Sending HUP");
	try {
	    process.kill(st.pid, 'SIGHUP');
	    last_signal_time = Date.now();
	} catch(e) {
	    console.error("Couldn't send SIGHUP", e);
	}
    }
}

var stats = null;
monitorSocket.on('message', function (msg, rinfo) {
    stats = JSON.parse(msg.toString());
    console.error('Process stats: ' + util.inspect(stats, true, null)); 
});

setInterval(function () { if (null != stats) checkHungProcess(stats.status) }, 1000);

fs.unlink(monPath, function () {
    var  um = process.umask(0);
    // Bind to the socket and start listening the stats
    monitorSocket.bind(monPath);
    setTimeout(function () {
        try {
            fs.chmodSync(monPath, 511); //0777
        } catch (e) {
            console.log("ERROR: Could not change mod for Socket" + e.stack);
        }
    }, 500);
    process.umask(um);
});

/*
 * Sample output on the console
 *
 * Process stats: { status: 
 *    { pid: 9560,
 *      ts: 947819135.59,
 *      cluster: 9560,
 *      reqstotal: 102,
 *      utcstart: 1379372564,
 *      debug: 0,
 *      events: 2,
 *      cpu: 0,
 *      mem: 0.36,
 *      cpuperreq: 0,
 *      oreqs: 0,
 *      sys_cpu: 0,
 *      oconns: 0,
 *      user_cpu: 0,
 *      rps: 0,
 *      kbs_out: 0,
 *      elapsed: 1.16,
 *      kb_trans: 16.54,
 *      jiffyperreq: 0 } }
 */
