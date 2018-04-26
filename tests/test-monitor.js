/* global describe,it,before,after */

var assert = require('assert'),
    async = require('async'),
    dgram = require('unix-dgram'),
    fs = require('fs'),
    http = require('http'),
    mkdirp = require('mkdirp'),
    monitor = require('../lib/monitor'),
    path = require('path');
var defaultIpcMonitorPath = '/tmp/nodejs.mon',
    ipcMonitorPath = '/tmp/nodejs-test-' + process.pid + '.mon';

var expectedProperties = [
    'cluster',
    'cpu',
    'cpuperreq',
    'elapsed',
    'events',
    'gc',
    'jiffyperreq',
    'kb_trans',
    'kbs_out',
    'mem',
    'oconns',
    'oreqs',
    'pid',
    'reqstotal',
    'rps',
    'sys_cpu',
    'ts',
    'user_cpu',
    'utcstart',
];

var expectedGCCollectors = [
    'marksweep',
    'scavenge',
];

var expectedGCStatProperties = [
    'count',
    'elapsed_ms',
    'max_ms',
];

describe('monitr', function() {
    describe('our own monitor path', function() {
        it('is settable', function() {
            assert.equal(defaultIpcMonitorPath, monitor.ipcMonitorPath);
            monitor.setIpcMonitorPath(ipcMonitorPath);
            assert.equal(ipcMonitorPath, monitor.ipcMonitorPath);
        });
    });

    describe('process.monitor', function() {
        before(function() {
            monitor.setIpcMonitorPath(ipcMonitorPath);
            monitor.start();
        });
        after(function() {
            monitor.stop();
        });

        it('is an object', function() {
            assert.equal(typeof process.monitor, 'object');
        });

        describe('gc property', function() {
            it('is an object', function() {
                assert.equal(typeof process.monitor.gc, 'object');
            });

            it('has count and elapsed properties', function() {
                assert.equal(typeof process.monitor.gc.count, 'number');
                assert.equal(typeof process.monitor.gc.elapsed, 'number');
                function getSetter( obj, prop ) {
                    return Object.getOwnPropertyDescriptor( obj, prop).set
                }
                assert.ok(undefined === getSetter(process.monitor.gc, 'count'));
                assert.ok(undefined === getSetter(process.monitor.gc, 'elapsed'));
                process.monitor.gc.count = 5;
                assert.notEqual(5, process.monitor.gc.count);
                // increase after a GC event
                global.gc();
                assert.notEqual(0, process.monitor.gc.count);
                assert.notEqual(0, process.monitor.gc.elapsed);
            });
        });
    });

    describe('message sent from a server', function() {
        var server, port;
        var socket;
        var topic = {};

        before(function(done) {
            async.series([
                (taskDone) => {
                    monitor.setIpcMonitorPath(ipcMonitorPath);
                    monitor.start();
                    taskDone();
                },
                (taskDone) => {
                    server = http.createServer(function(req, res) {
                        res.writeHead(200, {'Content-Type': 'text/plain'});
                        res.end('I am being monitored\n');
                    });
                    server.listen(0, () => {
                        port = server.address().port;
                        taskDone();
                    });
                },
                (taskDone) => {
                    http.get('http://127.0.0.1:' + port, (res) => {
                        res.on('data', () => {});   // drain response body
                        topic.totalRequests = process.monitor.getTotalRequestCount();
                        topic.requests = process.monitor.getRequestCount();
                        topic.openConnections = process.monitor.getOpenConnections();
                        topic.transferred = process.monitor.getTransferred();
                        taskDone();
                    }).on('error', taskDone);
                },
                (taskDone) => {
                    socket = dgram.createSocket('unix_dgram', function(msg) {
                        topic.msg = msg.toString();
                        // we just grab one message
                        taskDone();
                    });
                    var um = process.umask(0);
                    socket.bind(monitor.ipcMonitorPath);
                    process.umask(um);
                },
            ], done);
        });
        after(function(done) {
            // always do this cleanup
            async.series([
                (taskDone) => {
                    socket.close();
                    taskDone();
                },
                (taskDone) => {
                    server.close(taskDone);
                },
                (taskDone) => {
                    fs.unlink(monitor.ipcMonitorPath, taskDone);
                },
            ], done);
        });

        it('should return values', function() {
            assert.equal(1, topic.totalRequests);
            assert.equal(0, topic.requests);
            assert.notEqual(0, topic.transferred);
        });

        it('should have valid JSON', function() {
            var msg = JSON.parse(topic.msg);
            assert.ok(msg.hasOwnProperty('status'));
            var status = msg.status;
            assert.deepEqual(expectedProperties.sort(), Object.keys(status).sort());
            var gc = status.gc;
            assert.deepEqual(expectedGCCollectors.sort(), Object.keys(gc).sort());
            Object.keys(gc).forEach((coll) => {
                var fields = gc[coll];
                assert.deepEqual(expectedGCStatProperties.sort(), Object.keys(fields).sort());
            });
        });
    });

    describe('health check values', function() {
        var socket;
        var topic = {};

        this.timeout(20000);
        before(function(done) {
            async.series([
                (taskDone) => {
                    monitor.setIpcMonitorPath(ipcMonitorPath);
                    monitor.start();
                    process.monitor.setHealthStatus(true, 200);
                    taskDone();
                },
                (taskDone) => {
                    socket = dgram.createSocket('unix_dgram', function(msg) {
                        topic.isDown = process.monitor.isDown();
                        topic.statusCode = process.monitor.getStatusCode();
                        topic.statusTimestamp = process.monitor.getStatusTimestamp();
                        topic.statusDate = process.monitor.getStatusDate();
                        topic.msg = JSON.parse(msg.toString());
                        // wait for one with health status
                        if (topic.msg.status.health_status_code) {
                            taskDone();
                        }
                    });
                    var um = process.umask(0);
                    socket.bind(monitor.ipcMonitorPath);
                    process.umask(um);
                },
            ], done);
        });
        after(function(done) {
            // always do this cleanup
            async.series([
                (taskDone) => {
                    socket.close();
                    taskDone();
                },
                (taskDone) => {
                    fs.unlink(monitor.ipcMonitorPath, taskDone);
                },
            ], done);
        });

        it('has valid information', function() {
            assert.equal(true, topic.isDown);
            assert.equal(200, topic.statusCode);
            assert.equal(200, topic.msg.status.health_status_code);
            assert.equal(true, topic.msg.status.health_is_down);
            assert.equal(topic.statusTimestamp, topic.msg.status.health_status_timestamp);
            assert.ok(Date.now() - topic.statusDate.getTime() <= 2 * 60 * 1000);  // less than 2 min
            process.monitor.setHealthStatus(false, 300);
            assert.equal(false, process.monitor.isDown());
            assert.equal(300, process.monitor.getStatusCode());
            assert.ok(process.monitor.getStatusTimestamp() > topic.statusTimestamp);
        });
    });
});
