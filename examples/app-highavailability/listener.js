/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */
// Reference : http://yahooeng.tumblr.com/post/68823943185/nodejs-high-availability
var watcher = require('process-watcher');

/*
 * Dummy metric monitoring object.
 */
var watcher_metric = {
    /**
     * Increments metric
     */
    increment : function (name, v) {
        console.log('Increment ' + name + ' with ' + v);
    },
    /**
     * Set the metric or multiple metrics at the same time.
     */
    set : function (name, v) {
        console.log('Setting the following metrics: ' + require('util').inspect(name));
    }
};
var dgpath = '/tmp/nodejs.mon',
    statusPath = '/tmp/watcher_status_path_test',
    watcher_config = { max_inactive : 0.001, monitor : 0.001,  monPath: dgpath,
        timeout : 3, timeout_start : 60 };

//Instantiate watcher
var watcher_instance = new watcher.Watcher({ metric : watcher_metric, config : watcher_config });
