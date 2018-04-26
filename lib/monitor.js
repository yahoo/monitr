/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */
//Add process.monitor methods to compute the requests, open connections and data transferred
var EventEmitter = require('events').EventEmitter,
    net = require('net'),
    util = require('util');

var reqCounter, //Singleton instance to monitor requests, connections and data transferred
    healthStatus; //Singleton instance to monitor status of app

// Initialize an empty monitor object that hangs off the global
// nodejs process object.  Functions get added to this during
// initialization
process.monitor = process.monitor || {};

function ReqCounter() {
    this._requests = 0;
    this._totalRequests = 0;
    this._servers = [];
    this._connections = 0;
    this._transferred = 0;
}

util.inherits(ReqCounter, EventEmitter);

ReqCounter.prototype.addServer = function (server) {
    var found = false;
    this._servers.forEach(function (l) {
        if (l === server) {
            found = true;
        }
    });
    if (!found) {
        this._servers.push(server);
        this.registerEvents(server);
    }
};

ReqCounter.prototype.registerEvents = function (server) {
    var that = this,
        origEmit = server.emit,
        stream,
        req,
        resp,
        origEnd,
        origWrite;

    server.emit = function () {
        var args = arguments;
        if (args[0] !== 'connection' && args[0] !== 'request') {
            origEmit.apply(this, arguments);
            return;
        }

        if (args[0] === 'connection') {
            stream = args[1];
            origWrite = stream.write;

            // Add connection into the pool
            /*jslint plusplus:true*/
            that._connections++;
            /*jslint plusplus:false*/

            // get the number of bytes transferred
            stream.write = function (data) {
                // \todo consider listening on socket.end and aggregate
                // socket.bytesWritten/bytesRead rather than monkey-patching?
                // Otherwise we're just aggregating data returned, not
                // data read
                if (data && data.length) {
                    that._transferred += data.length;
                }
                origWrite.apply(this, arguments);
            };

            stream.on('close', function () {
                /*jslint plusplus:true*/
                that._connections--;
                /*jslint plusplus:false*/
                if (that._connections < 0) {
                    that._connections = 0;
                }
                that.emit('conn_closed', that._connections, that._requests);
            });

            that.emit('connection', that._connections, that._requests);

            // in the case of http server
        } else if (args[0] === 'request') {

            req = args[1];
            resp = args[2];
            origEnd = resp.end;

            /*jslint plusplus:true*/
            that._requests++;
            that._totalRequests++;
            that.emit('request', that._connections, that._requests, resp);

            resp.end = function () {
                that._requests--;
                if (that._requests < 0) {
                    that._requests = 0;
                }
                origEnd.apply(this, arguments);

                that.emit('req_end', that._connections, that._requests, req);
            };
            /*jslint plusplus:true*/
        }
        origEmit.apply(this, arguments);
    };
};

function HealthStatus() {
    this._isDown = false;
    this._statusCode = 0;
    this._timestamp = 0;

}
HealthStatus.prototype.setHealthStatus = function(isDown, statusCode) {
    this._isDown = Boolean(isDown);
    this._statusCode = parseInt(statusCode, 10);
    this._timestamp = Math.floor(Date.now()/1000); //convert to seconds
};

function setupHealthStatus() {

    healthStatus = healthStatus || new HealthStatus();

    process.monitor.setHealthStatus = function() {
        healthStatus.setHealthStatus.apply(healthStatus, arguments);
    };

    process.monitor.isDown = function() {
        return healthStatus._isDown;
    };

    process.monitor.getStatusCode = function() {
        return healthStatus._statusCode;
    };

    process.monitor.getStatusTimestamp = function() {
        return healthStatus._timestamp;
    };

    process.monitor.getStatusDate = function() {
        return new Date(healthStatus._timestamp * 1000);
    };

}


//Instantiate the request counter
//Add methods to process object
function setupReqCounter() {

    reqCounter = reqCounter || new ReqCounter();

    process.monitor.getRequestCount = function () {
        return reqCounter._requests;
    };

    process.monitor.getTotalRequestCount = function () {
        return reqCounter._totalRequests;
    };

    process.monitor.getTransferred = function () {
        return reqCounter._transferred;
    };

    process.monitor.getOpenConnections = function () {
        return reqCounter._connections;
    };
}


function toNumber(x) { return (x = Number(x)) >= 0 ? x : false; }

function isPipeName(s) {
  return typeof s === 'string' && toNumber(s) === false;
}

// Add servers to request counter
// This will enable counting of events like connections, requests etc
// on each of the servers in the application
function notifyServerListens(server) {
  if (reqCounter) {
    reqCounter.addServer(server);
  }
}

function setupListenHook() {
  var trueListen = net.Server.prototype.listen;
  net.Server.prototype.listen = function() {
    var self = trueListen.apply(this, arguments);
    if (!isPipeName(arguments[0])) {
      notifyServerListens(self);
    }
    return self;
  };
}

// Set up listeners to count metrics
setupListenHook();
setupReqCounter();
setupHealthStatus();


// Load the binary monitor module
// This has the start and stop methods to control monitoring
var monitor;
monitor = module.exports = require('bindings')('monitor.node');

// addServer is needed to allow adding new request handles to monitor
// in applications where this module is loaded after the server
// starts listening
module.exports.addServer = function(server) {
    reqCounter.addServer(server);
};
