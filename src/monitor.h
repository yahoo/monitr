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

#include <list>
#include <string>
#include <vector>

namespace ynode {

typedef struct {
    int init;
    long unsigned int utime_ticks;
    long int cutime_ticks;
    long unsigned int stime_ticks;
    long int cstime_ticks;

    long unsigned int cpu_total_time;
} CpuUsage;

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

/** POD structure that contains GC statistics for a given "interval"
 *  Times are in nanoseconds
 */
typedef struct {
    long unsigned int numCalls;
    uint64_t cumulativeTime; //< total time of all collections (ns)
    uint64_t maxTime;        //< max time of any single GC (ns)
} GCStat;

/**
 * Simple wrapper class to handle scoped lock for pre-existing libuv mutex
 */
class ScopedUVLock {
 public:
    ScopedUVLock(uv_mutex_t* m) : m_(m) { uv_mutex_lock(m_); }
    ~ScopedUVLock() { uv_mutex_unlock( m_ ); }
 private:
    uv_mutex_t* m_;

    ScopedUVLock();  //< can't create one of these without a reference to a mutex
};


/**
 * General interface for tracking garbage collection statistics/usage
 */
class GCUsageTracker {
public:
    GCUsageTracker() : totalCollections_(0), totalElapsedTime_(0) {};
    ~GCUsageTracker() {};

    //< Call this when a GC is starting
    void StartGC( const v8::GCType type ) {
        GetGCUsage(type).Start();
    }

    //< call when GC is ending
    void StopGC( const v8::GCType type ) {
        uint64_t elapsed = GetGCUsage(type).Stop();
        totalCollections_++;
        totalElapsedTime_ += elapsed;
    }

    const GCStat EndInterval( const v8::GCType type ) {
        return GetGCUsage( type ).EndInterval();
    }

    inline unsigned long int totalCollections() const { return totalCollections_; }
    inline uint64_t totalElapsedTime() const { return totalElapsedTime_; }

    static const int kNumGCTypes = 2;  //< must match # of distinct v8::GCTypes in v8.h

    static inline int v8GCTypeToIndex( const v8::GCType type ) {
        return type == v8::kGCTypeScavenge ? 0 : 1;
    }

    static inline v8::GCType indexTov8GCType( const int type ) {
        assert( type < kNumGCTypes );

        switch (type) {
        case 0:  return v8::kGCTypeScavenge;
        case 1:  return v8::kGCTypeMarkSweepCompact;
        default:
            return v8::kGCTypeAll;  //< with assertion above, this should be unreachable
        }
    }

    static const char* indexToString( const int type ) {
        assert( type < kNumGCTypes );

        switch (type) {
        case 0:  return "scavenge";
        case 1:  return "marksweep";
        default:
            return "unknown";  //< with assertion above, this should be unreachable
        }
    }

private:
    /**
     * Encapsulates garbage collection stats per "reporting interval"
     *
     * \desc There may be multiple calls to Start/Stop during each
     * interval.  We require a mutex lock to protect the stats_ data
     * since this is written to/updated by the v8 Javascript thread at
     * the same time as it may be read by the monitor thread
     **/
    class GCUsage {
    public:
        GCUsage();
        ~GCUsage();

        /** Returns garbage collection (GC) stats since last call to this function.
         *  Also resets the counters in order to start a new interval of GC stats
         */
        const GCStat EndInterval();

        /**
         * start and stop events get called from v8 gc prologue/epilogue
         **/
        void     Start();
        uint64_t Stop();   //< returns elapsed time as given by uv_hrtime()

    private:
        GCStat     stats_;
        uint64_t   startTime_;

        // lock used to prevent reading/writing GCStat data structure at the same time
        // by more than one thread
        uv_mutex_t  lock_;

        /* prevent copying/assigning */
        GCUsage(const GCUsage&);
        GCUsage& operator=(const GCUsage&);
    };

    inline GCUsage& GetGCUsage( const v8::GCType type ) {
        int collector = v8GCTypeToIndex( type );
        assert( collector >= 0 && collector < kNumGCTypes );

        return usage_[ v8GCTypeToIndex( type )];
    }

    GCUsage           usage_[kNumGCTypes];
    long unsigned int totalCollections_;
    uint64_t          totalElapsedTime_;
};

typedef struct {
    // time since last check
    volatile struct timeval lastTime_;

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
    // if divided by curTime - lastTime
    volatile int lastReqDelta_;

    // number of open requests
    volatile int currentOpenReqs_;

    // number of open connections
    volatile int currentOpenConns_;

    // Kb transfered since start
    volatile float lastKBytesTransfered_;
    volatile float lastKBytesSecond;

    // health status isDown
    volatile bool healthIsDown_;

    // health status: statusCode
    volatile int healthStatusCode_;

    volatile time_t healthStatusTimestamp_;

    volatile double pmem_;
} Statistics;


/** Singleton class that collects and sends stats about running isolate
 *
 * \todo This should be extended into a non-singleton
 * class that can be allocated one per v8 isolate, but for the
 * moment, it assumes there is only one running in the whole process
 * (currently true for default nodejs).
 **/
class NodeMonitor {
public:
    /** Initializes the singleton.  Do not call any other functions
     * unless this function has been called first 
     */
    static void Initialize(v8::Isolate* isolate);
         
    void Start();
    void Stop();
    virtual ~NodeMonitor();
  
    /** Returns isolate which this NodeMonitor object is monitoring
     *
     * 
     **/
    v8::Isolate* getIsolate() { return isolate_; };
    GCUsageTracker& getGCUsageTracker() { return gcTracker_; }

    bool sendReport();
    void setStatistics();
  
    static NodeMonitor& getInstance();

protected:
    // Constructor - protected since external clients must use getInstance()
    NodeMonitor(v8::Isolate* isolate);

private:
    void InitializeIPC();
    void InitializeProcessMonitorGCObject(); //< Install process.monitor.gc

    // Member variables
    bool running_;  //< have we already been started?

    time_t startTime;
  
    // Required to track CPU load asynchronously \todo no longer used?
    CpuUsageTracker cpuTracker_;

    // Required to track CPU load synchronously
    // in order to calculate the request per CPU ratio
    CpuUsageTracker cpuTrackerSync_;
    Statistics stats_;
  
    GCUsageTracker gcTracker_; //< keeps track of gc events
    pthread_t tmonitor_;    //< the pthread doing the monitoring
    v8::Isolate* isolate_;  //< the isolate this monitor is monitoring
  
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
    int ipcSocket_;
  
    static int getIntFunction(const char* funcName);
    static bool getBooleanFunction(const char* funcName);

    static NodeMonitor* instance_;  //< the singleton instance

    // null private copy/assignment constructors
    NodeMonitor(const NodeMonitor&);
    const NodeMonitor operator=(const NodeMonitor&);
};

}

#endif	/* MONITOR_H_ */
