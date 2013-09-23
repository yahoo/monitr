/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */

#ifndef MONITOR_H_
#define	MONITOR_H_

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <pthread.h>
#include <uv.h>
#include <v8.h>

#include <list>
#include <string>
#include <vector>

#define NODE_PROTECTED_PROPERTY(obj, name, getter, setter)                \
  obj->SetAccessor(v8::String::NewSymbol(name), getter, setter,           \
                   v8::Handle<v8::Value>(), PROHIBITS_OVERWRITING,        \
                    DontDelete)

#define NODE_PROT_RO_PROPERTY(obj, name, getter)                          \
  NODE_PROTECTED_PROPERTY(obj, name, getter, 0)

#define NODE_PROTECTED_METHOD(obj, name, callback)                        \
  obj->Set(v8::String::NewSymbol(name),                                   \
           v8::FunctionTemplate::New(callback)->GetFunction(),            \
           static_cast<v8::PropertyAttribute>(v8::ReadOnly|v8::DontDelete))

namespace node {

typedef struct {
    int init;
    long unsigned int utime_ticks;
    long int cutime_ticks;
    long unsigned int stime_ticks;
    long int cstime_ticks;

    long unsigned int cpu_total_time;
} CpuUsage;

typedef struct {
	
	// time since last check
	volatile struct timeval lastTime_;
	
	// last time delta (period between checks)
	volatile long timeDelta_;
	
	// time since last request
	volatile struct timeval timeSinceLastRequest_;
	
	// last RPS
	volatile int lastRPS_;
	
	// requests served
	volatile float lastJiffiesPerReq_;
	volatile float lastCpuPerReq_;
	
	// requests already served
	volatile int lastRequests_;
	
	// delta between last check essentially req/per second
	// if devided by curTime - lastTime 
	volatile int lastReqDelta_;
	
	// number of open requests
	volatile int currentOpenReqs_;
	
	// number of open connections
	volatile int currentOpenConns_;
	
	// Kb transfered since start
	volatile float lastKBytesTransfered_;
	volatile float lastKBytesSecond;
	volatile double pmem_;
	
} Statistics;

class CpuUsageTracker {
public:
    
    /**
     * Returns estimated CPU usage since last call to this function.
     */
    int GetCurrent(float* ucpu_usage, float* scpu_usage, long int * uticks, long int* sticks);
    CpuUsageTracker();
private: 
    int ReadCpuUsage(CpuUsage* result); 
    void CalculateCpuUsage(CpuUsage* cur_usage, CpuUsage* last_usage,
                    float* ucpu_usage, float* scpu_usage, long int * uticks, long int* sticks);
    CpuUsage lastUsage_; 
    CpuUsage currentUsage_; 
};


class NodeMonitor {
 public:
  static void Initialize();
  static void Stop();
  virtual ~NodeMonitor();
  
  static void ipcInitialization();
  static bool sendReport();
  static void setStatistics();
  
  static void shutdown();
  
 private:
  time_t startTime;
  
  // Required to track CPU load asyncronously
  CpuUsageTracker cpuTracker_;

  // Required to track CPU load syncronously
  // in order to calculate the request per CPU ration
  CpuUsageTracker cpuTrackerSync_;
  Statistics stats_;
  
  pthread_t tmonitor_;
  
  uv_async_t check_loop_;
  
  volatile unsigned int loop_count_;
  volatile unsigned int last_loop_count_;
  volatile uint64_t loop_timestamp_;
  volatile uint64_t start_timestamp_;
  volatile double consumption_;
  volatile int pending_;
  
  struct sockaddr_un ipcAddr_;
  socklen_t ipcAddrLen_;
  struct msghdr msg_;
  static NodeMonitor* instance_;
  int ipcSocket_;
  
  NodeMonitor();
  static unsigned int getIntFunction(const char* funcName);
};

}

#endif	/* MONITOR_H_ */
