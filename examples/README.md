# application
In one console start the main application
```
node monitor_me
```

# listener
In another console, build and invoke the status/health listener
```
npm install
node i_am_monitoring
```
The listener starts and will output the status/health messages received from the application

# Invoke
You can also invoke the application a couple of times to view updated stats
```
curl 'http://localhost:2000'

curl 'http://localhost:2000/fib?n=10'
```

# Output
```
  Process stats: { status: 
     { pid: 9560,
       ts: 947819135.59,
       cluster: 9560,
       reqstotal: 102,
       utcstart: 1379372564,
       debug: 0,
       events: 2,
       cpu: 0,
       mem: 0.36,
       cpuperreq: 0,
       oreqs: 0,
       health_status_timestamp: 1379372600,
       sys_cpu: 0,
       health_is_down: false,
       health_status_code: 0,
       oconns: 0,
       user_cpu: 0,
       rps: 0,
       kbs_out: 0,
       elapsed: 1.16,
       kb_trans: 16.54,
       jiffyperreq: 0 } }
```
