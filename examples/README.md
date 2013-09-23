# application
In one console start the main application
```
node monitor_me
```

# listener
In another console, build the listener and start it
```
ynpm install
node i_am_monitoring
```
Now you can see the listener starts receiving messages from the application.

# Invoke
Can also invoke server a couple of times to view updated stats
```
curl 'http://localhost:2000'
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
       sys_cpu: 0,
       oconns: 0,
       user_cpu: 0,
       rps: 0,
       kbs_out: 0,
       elapsed: 1.16,
       kb_trans: 16.54,
       jiffyperreq: 0 } }
```
