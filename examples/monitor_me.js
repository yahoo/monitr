/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */
var http = require('http'),
    url = require('url'),
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

function fib(n) {
  if (n<2)
    return 1;
  else
    return fib(n-2) + fib(n-1);
}

/*
 * Start a simple http server
 */
http.createServer(function (req, res) {
  res.writeHead(200, {'Content-Type': 'text/plain'});
  var parsedUrl = url.parse(req.url, true);
  if (parsedUrl.pathname == "/toggle-backtrace") {
      monitor.showBacktrace = ! monitor.showBacktrace;
      res.end('Toggling backtrace to ' + monitor.showBacktrace + '\n');
  }
  else if (parsedUrl.pathname == "/fib") {
    var param = parsedUrl.query.n || 20;
    var startTime = new Date().getTime();
    var result = fib(param);
    var endTime = new Date().getTime();
    res.end('Finished calculating fibonacci(' + param + ') = ' + result 
            + ' in ' + (endTime-startTime) + 'ms\n');
  }
  else {
    res.end('I am being monitored\n');
  }
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
