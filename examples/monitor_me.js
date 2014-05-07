/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */
var http = require('http'),
    monitor = require('..');

/* 
 * This is the main application that will be monitored
 * This is a simple HTTP Server
 * monitor.start will create a thread
 * and sends stats of the process to the socket
 * If there is a listener on the messages on the socket
 * The listener can get this process stats
 */
monitor.start();

/*
 * Start a simple http server
 */
http.createServer(function (req, res) {
  res.writeHead(200, {'Content-Type': 'text/plain'});
  res.end('I am being monitored\n');
  //send health info
  process.monitor.setHealthStatus(false,0);
}).listen(2000);

/*
 * stop monitoring
 */
process.on('exit', function () {
   monitor.stop();
});

// Graceful shutdown
process.on('SIGINT', function () {
   process.exit();
});
