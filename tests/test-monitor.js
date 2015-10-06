var dgram = require('unix-dgram'),
    fs = require('fs'),
    http = require('http'),
    monitor = require('../lib/monitor'),
    vows = require('vows'),
    assert = require('assert');

var defaultIpcMonitorPath = "/tmp/nodejs.mon",
    ipcMonitorPath = "/tmp/nodejs-test.mon";


// Set our own monitor path

assert.equal(defaultIpcMonitorPath, monitor.ipcMonitorPath);
monitor.setIpcMonitorPath(ipcMonitorPath);
assert.equal(ipcMonitorPath, monitor.ipcMonitorPath);

monitor.start();

// Create all the files that monitor module reads
// to gather process status

function mkdirs(dirpath, md) {
    var segs = dirpath.split('/'),
        mode = md || 511,
        i,
        success = false,
        lastError = null,
        dir,
        stat,
        len;

    if (segs[0]) {
        segs.unshift(process.cwd());
    } else {
        segs[0] = '/';
    }

    // iterate over path segments and create each of them
    for (i = 0, len = segs.length; i < len; ++i) {
        dir = segs.slice(0, i + 1).join('/');
        dir = dir[1] === '/' ? dir.slice(1) : dir;
        try {
            stat = fs.statSync(dir);
            if (!stat.isDirectory()) {
                throw new Error('Failed to mkdir "' + dir + '"');
            }
            if (i === len - 1) {
                success = true;
            }
        } catch (err) {
            if (err.code === 'ENOENT') {
                fs.mkdirSync(dir, mode);
                if (i === len - 1) {
                    success = true;
                }
            } else {
                lastError =  err;
            }
        }
    }
    if (!success) {
        if (!lastError) {
            lastError = new Error("unknown problem creating directory " + dirpath);
        }
        throw lastError;
    }
};

var expectedProperties = [
    'cluster',
    'pid',
    'cpu',
    'user_cpu',
    'sys_cpu',
    'cpuperreq',
    'jiffyperreq',
    'events',
    'elapsed',
    'ts',
    'mem',
    'reqstotal',
    'rps',
    'oreqs',
    'utcstart',
    'oconns',
    'kb_trans',
    'kbs_out',
    'gc'
];

var expectedGCCollectors = [
    'scavenge',
    'marksweep'
];

var expectedGCStatProperties = [
    'count',
    'elapsed_ms',
    'max_ms'
];

var server1;

var batch0 = {
    'Verify process.monitor': {
        topic: process.monitor,
        'is an object': function(topic) {
            assert.ok( "object" === typeof topic );
        },
        'with a gc property': function(topic) {
            assert.ok( topic.gc );
        },
        'gc property': {
            topic: function( monitor ) { return monitor.gc },
            'is an object': function(gc) {
                assert.ok("object" === typeof gc);
            },
            'has count and elapsed properties': {
                topic: function( gc ) { return gc },
                'which are numbers': function(gc) {
                    assert.ok('number' === typeof(gc.count));
                    assert.ok('number' === typeof(gc.elapsed));
                },
                'which are readonly': function(gc) {
                    function getSetter( obj, prop ) {
                        return Object.getOwnPropertyDescriptor( obj, prop).set
                    }
                    assert.ok( undefined === getSetter( gc, 'count' ) );
                    assert.ok(undefined === getSetter(gc,'elapsed'));
                    gc.count = 5;
                    assert.notEqual(5,gc.count);
                },
                'which increase after a GC event': function(gc) {
                    global.gc();
                    assert.notEqual(0, gc.count);
                    assert.notEqual(0, gc.elapsed);
                },
            }
        }
    }
};

var batch1 = {
    'Verify message sent from a server' : {
        topic: function () {
        var self = this,
            monitorSocket,
            msgObj,
            index,
            totalRequests,
            requests,
            openConnections,
            transferred;

        assert.equal(0, process.monitor.getTotalRequestCount());
        server1 = http.createServer(function (req, res) {
            res.writeHead(200, {'Content-Type': 'text/plain'});
            res.end('I am being monitored\n');
        });
        server1.listen(2000);

        http.get("http://127.0.0.1:2000", function(res) {
            res.on('data', function(chunk) { /* */ });  // drain
            totalRequests = process.monitor.getTotalRequestCount();
            requests = process.monitor.getRequestCount();
            openConnections = process.monitor.getOpenConnections();
            transferred = process.monitor.getTransferred();
        }).on('error', function (e) {
            console.error('Error ' + e.message);
        });
        setTimeout(function() {
            monitorSocket = dgram.createSocket('unix_dgram',function (msg, rinfo) {
                msg = msg.toString()
                monitorSocket.close();
                
                return self.callback(null, {
                        totalRequests: totalRequests,
                        requests: requests,
                        openConnections: openConnections,
                        transferred: transferred,
                        msg: msg
                });             
                
            });
            fs.unlink(monitor.ipcMonitorPath,
                function (err) {
                    if (err) {
                        console.log("Deleted socket with ERROR " + err.stack);
                    }
                    // get a directory and set a umask
                    var dir = require('path').dirname(monitor.ipcMonitorPath),
                        um = process.umask(0);

                    try {
                        mkdirs(dir, 511);
                    } catch (ex) {
                        console.log("ERROR: Failed to create directory for socket " + ex.stack);
                    }
                    monitorSocket.bind(monitor.ipcMonitorPath);
                    process.umask(um);
            });
        }, 1100);
        },
        'process.monitor functions should return values': function (topic) {
            assert.equal(1, topic.totalRequests);
            assert.equal(0, topic.requests);
            assert.notEqual(0, topic.transferred);
        },
        'should have valid JSON output': function (topic) {
            assert.doesNotThrow(function() { JSON.parse(topic.msg); }, Error);
            setTimeout(function() {}, 1);  // give sub-context based topics chance to run
        },
        'should have valid JSON message output': {
            topic: function(topic) {
                assert.doesNotThrow(function() { JSON.parse(topic.msg); }, Error);
                return JSON.parse(topic.msg);
            },
            'with all expected properties': function (msg) {
                assert.ok(msg.hasOwnProperty('status'));
                var status = msg.status;
                for (index = 0; index < expectedProperties.length; ++index) {
                    assert.ok(status.hasOwnProperty(expectedProperties[index]));
                }
            },
            'including gc stats': {
                topic: function (msg) { return msg.status.gc; },
                'for each collector': function (gc) {
                    for (index = 0; index < expectedGCCollectors.length; ++index) {
                        assert.ok(gc.hasOwnProperty(expectedGCCollectors[index]));
                    }
                },
                'with expected properties': function(gc) {
                    for (index = 0; index < expectedGCCollectors.length; ++index) {
                        var stats = gc[expectedGCCollectors[index]];
                        for (index2 = 0; index2 < expectedGCStatProperties.length; ++index2) {
                            assert.ok(stats.hasOwnProperty(expectedGCStatProperties[index2]));
                        }
                    }
                }
            }
        },
        teardown: function() {
            server1.close();
        }
    }
};

var batch2 = {
    'Verify health check values' : {
        topic : function() {
            var self = this;
            process.monitor.setHealthStatus(true,200);
            setTimeout(function() {
                monitorSocket = dgram.createSocket('unix_dgram',function (msg, rinfo) {
                    msgObj = JSON.parse(msg.toString());
                    monitorSocket.close();
                    return self.callback(null, {
                        isDown: process.monitor.isDown(),
                        statusCode: process.monitor.getStatusCode(),
                        timestamp: process.monitor.getStatusTimestamp(),
                        date : process.monitor.getStatusDate(),
                        msgObj: msgObj
                    });
                });
                fs.unlink(monitor.ipcMonitorPath,
                          function (err) {
                              if (err) {
                                  console.log("Deleted socket with ERROR " + err.stack);
                              }
                              // get a directory and set a umask
                              var dir = require('path').dirname(monitor.ipcMonitorPath),
                                  um = process.umask(0);
                              try {
                                  mkdirs(dir, 511);
                              } catch (ex) {
                                  console.log("ERROR: Failed to create directory for socket " + ex.stack);
                              }
                              monitorSocket.bind(monitor.ipcMonitorPath);
                              process.umask(um);
                          });
            }, 1100);
        },
        'validate health information': function (topic) {
            assert.equal(true, topic.isDown);
            assert.equal(200, topic.statusCode);
            assert.equal(200, topic.msgObj.status.health_status_code);
            assert.equal(true, topic.msgObj.status.health_is_down);
            assert.equal(topic.timestamp, topic.msgObj.status.health_status_timestamp);
            assert.ok(Date.now() - topic.date.getTime() <= 2 * 60 * 1000); //less than 2 min

            process.monitor.setHealthStatus(false,300);
            assert.equal(false, process.monitor.isDown());
            assert.equal(300, process.monitor.getStatusCode());
            assert.ok(process.monitor.getStatusTimestamp() > topic.timestamp);
        }
    },
};

process.on('exit', function () {
    monitor.stop();
});

vows.describe('monitor')
    .addBatch(batch0)
    .addBatch(batch1)
    .addBatch(batch2)
    .export(module);
