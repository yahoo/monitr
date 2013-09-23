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
    'kbs_out'
];

var tests = {
    'Verify message is sent in a server' : {
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
        var server1 = http.createServer(function (req, res) {
            res.writeHead(200, {'Content-Type': 'text/plain'});
            res.end('I am being monitored\n');
        })
        server1.listen(2000);
        var server2 = http.createServer(function (req, res) {
            res.writeHead(200, {'Content-Type': 'text/plain'});
            res.end('I am being monitored\n');
        })
        server2.listen(3000);
        server2.listen(3000);


        http.get("http://127.0.0.1:2000", function(res) {
	    totalRequests = process.monitor.getTotalRequestCount();
	    requests = process.monitor.getRequestCount();
	    openConnections = process.monitor.getOpenConnections();
	    transferred = process.monitor.getTransferred();
        }).on('error', function (e) {
            console.error('Error ' + e.message);
        });

        monitorSocket = dgram.createSocket('unix_dgram',
            function (msg, rinfo) {
                console.log('message: ' + msg.toString());
                msgObj = JSON.parse(msg.toString());
		monitorSocket.close();
		return self.callback(null, {
                    totalRequests: totalRequests,
		    requests: requests,
		    openConnections: openConnections,
		    transferred: transferred,
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
        },
        'should have all expected properties': function (topic) {
	    assert.ok(topic.msgObj.hasOwnProperty('status'));
	    var status = topic.msgObj.status;
	    for (index = 0; index < expectedProperties.length; ++index) {
                assert.ok(topic.msgObj.status.hasOwnProperty(expectedProperties[index]));
            }
        },
	'process.monitor functions should return values': function (topic) {
	    assert.equal(1, topic.totalRequests);
	    assert.equal(0, topic.requests);
	    assert.equal(1, topic.openConnections);
	    assert.ok(topic.transferred !== 0);
	}
    }
};

process.on('exit', function () {
    monitor.stop();
});

vows.describe('monitor').addBatch(tests).export(module);
