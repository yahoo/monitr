/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */

// Reference: http://yahooeng.tumblr.com/post/68823943185/nodejs-high-availability

var cluster = require('cluster'),
    express = require('express'),
    http = require('http'),
    monitor = require('monitr'),
    status = require('mod_statuspage'),
    timers = require('timers');
// Spawn numCPUs worker processes
var numCPUs = require('os').cpus().length,
    clusterStartTime = Date.now(),
    newWorkerEnv = {};

// Master spawns worker processes
if (cluster.isMaster) {
  newWorkerEnv.clusterStartTime = clusterStartTime;

  // Fork workers
  for (var i = 0; i < numCPUs; i++) {
     console.log('Starting worker ' + i);
     cluster.fork(newWorkerEnv);
  }
  
  // If any worker process dies, spawn a new one to bring back number of processes 
  // to be back to numCPUs
  cluster.on('exit', function(worker, code, signal) {
     console.log('worker ' + worker.process.pid + ' died');
     cluster.fork(newWorkerEnv);
  });

  // Logs to know what workers are active
  cluster.on('online', function (worker) {
     console.log('Worker ' + worker.process.pid + ' online');
  });

} else {
    // Worker process
    // Start monitoring
    monitor.start();

    if (process.env.clusterStartTime) {
        process.clusterStartTime = new Date(parseInt(process.env.clusterStartTime,10));
    }
    
    // Simple express app
    var app = express();
    // Server statuspage to get application metrics
    app.use(status({
        url: '/status',
        check: function(req) {
            if (req.something == false) {
                return false; //Don't show status
            }
            return true; //Show status
        },
        responseContentType : 'html'
    }));
    // An example of a request that makes CPU idle for some time
    app.get('/late-response', function(req, res) {
        var end = Date.now() + 5000
        // CPU cycles wasted for 5000ms
        // Blocking
        while (Date.now() < end) ;
        res.writeHead(200, {'Content-Type': 'text/plain'});
        res.end('I came after a delay\n');
    });
    // All other requests served normal
    app.get('*', function (req, res) {
      res.writeHead(200, {'Content-Type': 'text/plain'});
      res.end('I am being monitored\n');
    });
    
    console.log('Go to: http://127.0.0.1:8000/status');
    app.listen(8000);
    
    // Stop monitoring
    process.on('exit', function () {
       monitor.stop();
    });
    
    // Graceful shutdown
    process.on('SIGINT', function () {
       process.exit();
    });
} // end worker
