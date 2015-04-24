/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */

#include <sys/syscall.h>

#include <sys/types.h>
#include <sys/select.h>
#include <sys/prctl.h>  // to set thread name
#include <dirent.h>
#include <math.h>
#include <node.h>
#include <node_internals.h>
#include <v8.h>
#include <v8-debug.h>
#include <unistd.h>

#define _WIN32 1
#define _USING_UV_SHARED 1
#ifdef BUILDING_UV_SHARED
#undef BUILDING_UV_SHARED
#endif

#include <uv.h>

#include "monitor.h"

#include <iostream>
#include <fstream>
#include <sys/time.h>
#include <algorithm>
#include <signal.h>


#ifdef __APPLE__
    #include <sys/sysctl.h>
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#else
    extern char **environ;
#endif

#define THROW_BAD_ARGS() \
    NanThrowError(Exception::TypeError(NanNew<String>(__FUNCTION__)));


using namespace std;
using namespace v8;


// This is the default IPC path where the stats are written to
// Could use the setter method to change this
static string _ipcMonitorPath = "/tmp/nodejs.mon";
static bool _show_backtrace = false;  //< default to false to avoid performance
static const int MAX_INACTIVITY_RETRIES = 5;
// default is to show a backtrace when receiving a HUP
static const int REPORT_INTERVAL_MS = 1000;

/* globals used for signal catching, etc */
static volatile sig_atomic_t hup_fired = 0;
static siginfo_t savedSigInfo;
static sigset_t savedBlockSet;
static int sigpipefd_w = -1;
static int sigpipefd_r = -1;


namespace ynode {

// our singleton instance
NodeMonitor* NodeMonitor::instance_ = NULL;

void RegisterSignalHandler(int signal, void (*handler)(int, siginfo_t *, void *)) {

    sigset_t blockset;
    sigemptyset(&blockset);  
    sigaddset(&blockset, SIGHUP);
    // block SIGHUP until we get to pselect() call to avoid race condition
    // See http://lwn.net/Articles/176911/
    sigprocmask(SIG_BLOCK, &blockset, &savedBlockSet);  

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO; // Tell sigaction() to use the sa_sigaction field, not sa_handler.
    sigemptyset(&sa.sa_mask); // Allow other signals to run during SIGHUP handler
    if (sigaction(signal, &sa, NULL) < 0) {
        perror("sigaction");
    }
}

/** Sleep waiting for some event on our pipe from the signal handler
 *  \param ms max number of milliseconds to wait
 **/
static void doSleep(int ms) {
    int res;
    struct timespec timeout;

    timeout.tv_sec = ms / 1000;
    timeout.tv_nsec = (ms % 1000) * 1000 * 1000;

    // put this thread to sleep until timeout, or a SIGHUP occurs
    sigset_t blockset;
    blockset = savedBlockSet;
    // ensure we don't block SIGHUP in pselect in case signal delivered to this thread
    sigdelset(&blockset, SIGHUP);

    fd_set readfds;
    FD_ZERO(&readfds );
    FD_SET(sigpipefd_r, &readfds);

    res = pselect(sigpipefd_r + 1, &readfds, NULL, NULL, &timeout, &blockset);

    // Did we exit due to a "signal" sent via the pipe (likely from another thread)
    if (res >= 0) {
        if (FD_ISSET(sigpipefd_r, &readfds)) {
            char c[100];
            while ((read(sigpipefd_r, &c, sizeof(c))) == sizeof(c)) {
              /* Flush the pipe! */
            }
        }
    }
    else {
        perror("pselect");
    }
}

/*
 * Thread which reports the
 * status of the current process to the watcher
 * process.
 */
void* monitorNodeThread(void *arg) {
    int errorCounter = 0;

    // set the thread name so it's easy to distinguish when debugging
    int rc = pthread_setname_np(pthread_self(), "monitr");
    if (0 != rc) perror("pthread_setname_np");

    NodeMonitor& monitor = NodeMonitor::getInstance();
    doSleep(REPORT_INTERVAL_MS);
    while (true) {
        if (hup_fired) {
            siginfo_t *siginfo = &savedSigInfo;
            std::cout << "Process " << getpid() << " received SIGHUP from Process (pid: "
                      << siginfo->si_pid << " uid: " << siginfo->si_uid << ")" << std::endl;

            // We are calling DebugBreak from our monitor thread, which
            // is running independently of the main v8 thread.
            // Calling DebugBreak() from outside the thread is allowed, but
            // setting a DebugEventListener() is *not*.
            // v8 will try to make a debugger event callback when the next
            // StackGuard check occurs (i.e. start/end functions, back loops)

            if (_show_backtrace) {
                // Only attempt to call DebugBreak() if we have the
                // DebugEventHandler installed, since otherwise v8
                // will *never* clear the DEBUGBREAK flag in the
                // StackGuard thread_local inside v8
                v8::Debug::DebugBreak(monitor.getIsolate());
            }
            hup_fired = 0;
        }
        if (!errorCounter) {
            if (!monitor.sendReport()) {
                // slow down reporting if noone is listening
                ++errorCounter;
            }
        } else {
            ++errorCounter;
            if (errorCounter >= MAX_INACTIVITY_RETRIES) {
                errorCounter = 0;
            }
        }
        doSleep(REPORT_INTERVAL_MS);
    }
    exit(0);
}

NAUV_WORK_CB(updateLoopTimeStamp) {
    NodeMonitor::getInstance().setStatistics();
}

static NAN_GC_CALLBACK(startGC) {
    NodeMonitor::getInstance().getGCUsageTracker().GetGCUsage(type)->Start();
}

static NAN_GC_CALLBACK(stopGC) {
    NodeMonitor::getInstance().getGCUsageTracker().GetGCUsage(type)->Stop();
}

static void InstallGCEventCallbacks() {
    NanAddGCPrologueCallback(startGC);
    NanAddGCEpilogueCallback(stopGC);
}

static void UninstallGCEventCallbacks() {
    NanRemoveGCPrologueCallback(startGC);
    NanRemoveGCEpilogueCallback(stopGC);
}

/**
 * Set up the singleton instance variable
 */
void NodeMonitor::Initialize(v8::Isolate* isolate) {

    // only one instance is allowed per process
    // \todo change this to be one per *isolate*
    if (instance_) {
        return;
    }
    assert(0 != isolate);
    instance_ = new NodeMonitor(isolate);
}

/**
 * Spawns a thread to monitor stats every REPORT_INTERVAL_MS
 */
void NodeMonitor::Start() {

    assert( 0 != instance_ );

    InstallGCEventCallbacks();

    ipcSocket_ = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (ipcSocket_ != -1) {
        fcntl(ipcSocket_, F_SETFD, FD_CLOEXEC);
    }

    /* Use a pipe to let the signal handler (which will likely be 
       executed in another thread) break out of pselect().
       This is the standard DJ Bernstein pipe for handling signals
       in multi-thread programs technique - http://cr.yp.to/docs/selfpipe.html */
    {
        /* \todo - make these local to the NodeMonitor rather than global */
        int fd[2];
        if (pipe(fd)) {
            perror("Can't create pipe");
        }
        sigpipefd_r = fd[0]; 
        sigpipefd_w = fd[1];
        fcntl(sigpipefd_r, F_SETFL, fcntl(sigpipefd_r, F_GETFL) | O_NONBLOCK );
        fcntl(sigpipefd_r, F_SETFD, FD_CLOEXEC );
        fcntl(sigpipefd_w, F_SETFL, fcntl(sigpipefd_w, F_GETFL) | O_NONBLOCK );
        fcntl(sigpipefd_w, F_SETFD, FD_CLOEXEC );
    }


    // Set it up such that libuv will execute our callback function
    // (updateLoopTimeStamp) each time through the default uv event loop
    uv_async_init(uv_default_loop(), &check_loop_, updateLoopTimeStamp);
    uv_unref((uv_handle_t*) &check_loop_);

    ipcInitialization();
    {
        int rc;
        rc = pthread_create(&tmonitor_, NULL, monitorNodeThread, NULL);
        if (0 != rc) perror("pthread_create");
    }
}


/**
 * Update statistics each time through the libuv event loop
 *
 * This gets called in the context of the node/v8 execution thread, so
 * it's no problem to call v8 specific APIs which may, in turn, invoke
 * their own callbacks into Javascript (e.g. the getIntFunction()
 * examples here)
 **/
void NodeMonitor::setStatistics() {

    pending_ = 0;
    loop_timestamp_ = uv_hrtime();
    loop_count_++;

    // obtain heap memory usage ratio
    v8::HeapStatistics v8stats;
    NanGetHeapStatistics(&v8stats);

    double pmem = (v8stats.used_heap_size() / (double) v8stats.total_heap_size());

    // Obtains the CPU usage
    float scpu = 0.0;
    float ucpu = 0.0;
    long int uticks = 0;
    long int sticks = 0;

    cpuTrackerSync_.GetCurrent(&ucpu, &scpu, &uticks, &sticks);

    // Get current number of requests
    unsigned int currReqs = getIntFunction("getTotalRequestCount");
    unsigned int reqDelta = currReqs - stats_.lastRequests_;

    // Get the current time
    struct timeval cur_time = { 0, 0 };
    gettimeofday(&cur_time, NULL);

    // milliseconds
    long timeDelta = (cur_time.tv_sec * 1000 + cur_time.tv_usec / 1000)
        - (stats_.lastTime_.tv_sec * 1000 + stats_.lastTime_.tv_usec / 1000);

    // Update the number of requests processed
    // and the ratio CPU/req.
    stats_.lastRequests_ = currReqs;
    stats_.lastCpuPerReq_ = (reqDelta <= 0) ? 0 : (scpu + ucpu) / reqDelta;
    stats_.lastJiffiesPerReq_ = (reqDelta <= 0) ? 0
        : ((float) (sticks + uticks)) / reqDelta;

    // Request delta - requests since last check.
    stats_.lastReqDelta_ = reqDelta;
    stats_.timeDelta_ = timeDelta;

    // Update time
    stats_.lastTime_.tv_sec = cur_time.tv_sec;
    stats_.lastTime_.tv_usec = cur_time.tv_usec;

    // Last RPS
    stats_.lastRPS_ = (int) (reqDelta / (((double) timeDelta) / 1000));

    // Get currently open requests
    stats_.currentOpenReqs_ = getIntFunction("getRequestCount");

    // currently open connections
    stats_.currentOpenConns_ = getIntFunction("getOpenConnections");

    // Kb of transferred data
    float dataTransferred = ((float) (getIntFunction("getTransferred"))) / 1024;

    stats_.lastKBytesSecond = (dataTransferred - stats_.lastKBytesTransfered_) / (((double) timeDelta) / 1000);
    stats_.lastKBytesTransfered_ = dataTransferred;
    stats_.healthIsDown_ = getBooleanFunction("isDown");
    stats_.healthStatusCode_ = getIntFunction("getStatusCode");
    stats_.healthStatusTimestamp_ = (time_t) getIntFunction("getStatusTimestamp");

    stats_.pmem_ = pmem;
}

void NodeMonitor::ipcInitialization() {
    memset(&ipcAddr_, 0, sizeof(ipcAddr_));
    ipcAddr_.sun_family = AF_UNIX;

    strncpy(ipcAddr_.sun_path, _ipcMonitorPath.c_str(),
            sizeof(ipcAddr_.sun_path));
    ipcAddrLen_ = sizeof(ipcAddr_.sun_family) + strlen(ipcAddr_.sun_path) + 1;

    memset(&msg_, 0, sizeof(msg_));

    msg_.msg_name = &ipcAddr_;
    msg_.msg_namelen = ipcAddrLen_;
    msg_.msg_iovlen = 1;
}

CpuUsageTracker::CpuUsageTracker() {
    memset(&lastUsage_, 0, sizeof(CpuUsage));
    memset(&currentUsage_, 0, sizeof(CpuUsage));

    // Read it once, to be able to compare
    float ucpu_usage = 0.0f, scpu_usage = 0.0f;
    long int uticks, sticks;
    GetCurrent(&ucpu_usage, &scpu_usage, &uticks, &sticks);
}

int CpuUsageTracker::GetCurrent(float* ucpu_usage, float* scpu_usage, long int * uticks, long int* sticks) {
    int err = 0;
    if (!(err = ReadCpuUsage(&currentUsage_))) {
        *ucpu_usage = 0.0f;
        *scpu_usage = 0.0f;
        *uticks = 0;
        *sticks = 0;

        if (lastUsage_.init) {
            CalculateCpuUsage(&currentUsage_, &lastUsage_, ucpu_usage, scpu_usage, uticks, sticks);
        }

        // Copy the results
        lastUsage_ = currentUsage_;
        lastUsage_.init = true;
    }
    return err;
}

// return 0 on success, -1 on error
int CpuUsageTracker::ReadCpuUsage(CpuUsage* result) {

    //convert  pid to string
    char pid_s[20];
    pid_t pid = getpid();

    snprintf(pid_s, sizeof(pid_s), "%d", pid);
    char stat_filepath[30] = "/proc/";
    strncat(stat_filepath, pid_s, sizeof(stat_filepath) - strlen(stat_filepath) - 1);
    strncat(stat_filepath, "/stat", sizeof(stat_filepath) - strlen(stat_filepath) - 1);

    //open /proc/pid/stat
    FILE *fpstat = fopen(stat_filepath, "r");
    if (fpstat == NULL) {
        printf("FOPEN ERROR pid stat %s:\n", stat_filepath);
        return -1;
    }

    //open /proc/stat
    FILE *fstat = fopen("/proc/stat", "r");
    if (fstat == NULL) {
        printf("FOPEN ERROR");
        fclose(fstat);
        return -1;
    }
    memset(result, 0, sizeof(CpuUsage));

    //read values from /proc/pid/stat
    if (fscanf(
        fpstat,
        "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %ld %ld",
        &result->utime_ticks, &result->stime_ticks, &result->cutime_ticks,
        &result->cstime_ticks) == EOF) {
        fclose(fpstat);
        fclose(fstat);
        return -1;
    }
    fclose(fpstat);

    //read+calc cpu total time from /proc/stat, on linux 2.6.35-23 x86_64 the cpu row has 10values could differ on different architectures :/
    long unsigned int cpu_time[10] = { 0 };
    memset(cpu_time, 0, sizeof(cpu_time));
    if (fscanf(fstat, "%*s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
        &cpu_time[0], &cpu_time[1], &cpu_time[2], &cpu_time[3],
        &cpu_time[4], &cpu_time[5], &cpu_time[6], &cpu_time[7],
        &cpu_time[8], &cpu_time[9]) == EOF) {
        fclose(fstat);
        return -1;
    }
    fclose(fstat);

    for (int i = 0; i < 10; i++) {
        result->cpu_total_time += cpu_time[i];
    }

    return 0;
}

void CpuUsageTracker::CalculateCpuUsage(CpuUsage* cur_usage,
    CpuUsage* last_usage, float* ucpu_usage, float* scpu_usage,
    long int * uticks, long int* sticks) {
    long unsigned int curTotalDiff = cur_usage->cpu_total_time - last_usage->cpu_total_time;

    if (curTotalDiff > 0) {
        *ucpu_usage = 100 * ((((cur_usage->utime_ticks
            + cur_usage->cutime_ticks) - (last_usage->utime_ticks
            + last_usage->cutime_ticks))) / ((float) curTotalDiff));

        *scpu_usage = 100 * ((((cur_usage->stime_ticks
            + cur_usage->cstime_ticks) - (last_usage->stime_ticks
            + last_usage->cstime_ticks))) / ((float) curTotalDiff));
    } else {
        *ucpu_usage = 0.0f;
        *scpu_usage = 0.0f;
    }
    *uticks = ((cur_usage->utime_ticks + cur_usage->cutime_ticks)
        - (last_usage->utime_ticks + last_usage->cutime_ticks));
    *sticks = ((cur_usage->stime_ticks + cur_usage->cstime_ticks)
        - (last_usage->stime_ticks + last_usage->cstime_ticks));
}

Local<Value> callFunction(const char* funcName) {
    NanEscapableScope();
    
    Local<Value> pr = NanGetCurrentContext()->Global()->Get(NanNew<String>("process"));

    if (pr->IsObject()) {
        Local<Value> exten = pr->ToObject()->Get(NanNew<String>("monitor"));
        if (exten->IsObject()) {
            Local<Value> fval = exten->ToObject()->Get(NanNew<String>(funcName));
            if (fval->IsFunction()) {
                Local<Function> fn = Local<Function>::Cast(fval);
                Local<Value> argv[1];
                argv[0] = NanNew(NanNull());
                return  NanEscapeScope(fn->Call(NanGetCurrentContext()->Global(), 1, argv));
            }
        }
    }
    return NanEscapeScope(NanNew(NanNull()));
        
}


GCUsage::GCUsage() {
    int rc = uv_mutex_init(&lock_);
    if (0 != rc) {
        perror("GCUsage: could not initialize uv_mutex");
    }

    bzero( &stats_, sizeof(GCStat) );
    startTime_ = 0;
}

GCUsage::~GCUsage() {
    uv_mutex_destroy(&lock_);
}

void GCUsage::Start() {
    startTime_ = uv_hrtime();
}

void GCUsage::Stop() {
    assert(0 != startTime_);
    uint64_t elapsed = uv_hrtime() - startTime_;
    {
        ScopedUVLock scope( &lock_ );

        stats_.numCalls++;
        stats_.cumulativeTime += elapsed;
        if (elapsed > stats_.maxTime) {
            stats_.maxTime = elapsed;
        }
    }
}


const GCStat GCUsage::EndInterval() {
    GCStat lastStat;
    {
        ScopedUVLock scope( &lock_ );

        lastStat = stats_;
        // now clear out stats since we've saved the last read
        bzero( &stats_, sizeof(GCStat) );

        // but don't clear out the startTime_ since we may be in
        // the middle of another GC when this EndInterval is called
        // by the profiling thread
    }
    return lastStat;
}

// calls the function which return the Int value
int NodeMonitor::getIntFunction(const char* funcName) {
    NanScope();
    Local<Value> res = callFunction(funcName);
    if (res->IsNumber()) {
        return res->Uint32Value();
    }
    return 0;
}
    
bool NodeMonitor::getBooleanFunction(const char* funcName) {
    NanScope();
    Local<Value> res = callFunction(funcName);
    if (res->IsBoolean()) {
        return res->BooleanValue();
    }
    return false;
}

NodeMonitor& NodeMonitor::getInstance() {
    assert( 0 != instance_ );
    return *instance_;
}

/**
 * Sends udp JSON datagram approximately once per REPORT_INTERVAL_MS
 *
 * Executed in the monitr pthread, *not* from any pthread executing v8
 * Therefore, we can't directly access any Javascript functions/vars
 **/
bool NodeMonitor::sendReport() {
    static pid_t pid = getpid();
    static double minOverHead = 0;

    // See how many reports have been processed since last call to this function
    unsigned int diff_count = loop_count_ - last_loop_count_ + 1;
    last_loop_count_ = loop_count_;

    // The different between current and previous time (in miliseconds)
    double ts_diff = (loop_timestamp_ - start_timestamp_) / 1.0e6;

    // Obtains the CPU usage
    float scpu = 0.0f;
    float ucpu = 0.0f;
    float cpusum = 0.0f;
    long int uticks = 0;
    long int sticks = 0;

    cpuTracker_.GetCurrent(&ucpu, &scpu, &uticks, &sticks);

    cpusum = ucpu + scpu;

    // Obtain the time elapsed since last event
    if (ts_diff > 0) {
        if (minOverHead == 0.0 || minOverHead > ts_diff) {
            minOverHead = ts_diff;
        }
    } else {
        ts_diff = -ts_diff;
    }

    consumption_ = cpusum;
    Statistics& stats = stats_;

    const int k_MAX_BUFLENGTH = 100;
    char buffer[k_MAX_BUFLENGTH];

    // data must result in a valid JSON object, although we don't validate this!
    string data = "{\"status\":{";
    snprintf(buffer, sizeof(buffer), "\"cluster\":%d,", getpgid(0));
    data.append(buffer);
    snprintf(buffer, sizeof(buffer), "\"pid\":%d,", pid);
    data.append(buffer);
    snprintf(buffer, sizeof(buffer), "\"cpu\":%.2f,", ucpu + scpu);

    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"user_cpu\":%.2f,", ucpu);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"sys_cpu\":%.2f,", scpu);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"cpuperreq\":%.6f,", stats.lastCpuPerReq_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"jiffyperreq\":%.6f,", stats.lastJiffiesPerReq_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }	

    snprintf(buffer, sizeof(buffer), "\"events\":%d,", diff_count);
    data.append(buffer);

    snprintf(buffer, sizeof(buffer), "\"elapsed\":%.2f,", ts_diff);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"ts\":%.2f,", loop_timestamp_ / 1.0e6);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    // memory
    snprintf(buffer, sizeof(buffer), "\"mem\":%.2f,", stats.pmem_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    // requests served since beginning 
    snprintf(buffer, sizeof(buffer), "\"reqstotal\":%d,", stats.lastRequests_);
    data.append(buffer);

    // RPS
    snprintf(buffer, sizeof(buffer), "\"rps\":%d,", stats.lastRPS_);
    data.append(buffer);

    // open requests
    snprintf(buffer, sizeof(buffer), "\"oreqs\":%d,", stats.currentOpenReqs_);
    data.append(buffer);

    // startTime of the process.
    snprintf(buffer, sizeof(buffer), "\"utcstart\":%d,", (int) startTime);
    data.append(buffer);

    // open connections
    snprintf(buffer, sizeof(buffer), "\"oconns\":%d,", stats.currentOpenConns_);
    data.append(buffer);

    // Kb transferred
    snprintf(buffer, sizeof(buffer), "\"kb_trans\":%.2f,", stats.lastKBytesTransfered_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    // Kb transferred per second
    snprintf(buffer, sizeof(buffer), "\"kbs_out\":%.2f,", stats.lastKBytesSecond);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }	

    if (stats.healthStatusTimestamp_ != 0) {
        snprintf(buffer, sizeof(buffer), "\"health_status_timestamp\":%ld,", stats.healthStatusTimestamp_);
        data.append(buffer);

        //Add the rest health statistics only if health timestamp is not 0
        snprintf(buffer, sizeof(buffer), "\"health_is_down\":%s,", (stats.healthIsDown_ ? "true" : "false"));
        data.append(buffer);

        snprintf(buffer, sizeof(buffer), "\"health_status_code\":%d,", stats.healthStatusCode_);
        data.append(buffer);
    }

    // append gc stats
    {
        GCUsageTracker& tracker = getGCUsageTracker();

        snprintf(buffer, sizeof(buffer), "\"gc\":{" );
        data.append(buffer);

        for (int i=0; i < GCUsageTracker::kNumGCTypes; ++i) {
            v8::GCType type = GCUsageTracker::indexTov8GCType(i);
            const GCStat stat = tracker.GetGCUsage( type )->EndInterval();

            snprintf(buffer, sizeof(buffer), "\"%s\":{", GCUsageTracker::indexToString(i) );
            data.append(buffer);

            snprintf(buffer, sizeof(buffer), "\"count\":%lu,\"elapsed_ms\":%1.3f,\"max_ms\":%1.3f},",
                     stat.numCalls, stat.cumulativeTime / (1000 * 1000.0), stat.maxTime / (1000*1000.0) );
            data.append(buffer);
        }

        data.erase(data.size() - 1);; //get rid of last comma

        // end the object literal
        snprintf(buffer, sizeof(buffer), "}," );
        data.append(buffer);

    }

    data.erase(data.size() - 1);; //get rid of last comma
    
    data.append("}}");

    // Send datagram notification to the listener
    // If Any
    struct iovec vec;

    vec.iov_base = (void *) data.c_str();
    vec.iov_len = strlen((char *) vec.iov_base);
    msg_.msg_iov = &vec;
    int rc = sendmsg(ipcSocket_, &msg_, MSG_DONTWAIT);

    if (!pending_) {
        start_timestamp_ = uv_hrtime();
        pending_ = 1;
    }
    uv_async_send(&check_loop_);
    return (rc != -1);
}

void NodeMonitor::Stop() {
    assert( 0 != instance_ );

    UninstallGCEventCallbacks();

    pthread_cancel(tmonitor_);
    close(ipcSocket_);
}

NodeMonitor::NodeMonitor(v8::Isolate* isolate) :
    startTime(0),
    gcTracker_(),
    tmonitor_((pthread_t) NULL),
    isolate_(isolate),
    loop_count_(0),
    last_loop_count_(0),
    consumption_(0.0),
    pending_(0),
    ipcAddrLen_(0),
    ipcSocket_(-1)
{
    loop_timestamp_ = start_timestamp_ = uv_hrtime();
    startTime = time(NULL);
    memset(&stats_, 0, sizeof(Statistics));
    memset(&ipcAddr_, 0,sizeof(struct sockaddr_un));
}

NodeMonitor::~NodeMonitor() {
}


void LogStackTrace(Handle<Object> obj) {
    try {
        Local<Value> args[] = {};
        Local<Value> frameCount = obj->Get(NanNew<String>("frameCount"));
        Local<Function> frameCountFunc = Local<Function>::Cast(frameCount);
        Local<Value> frameCountVal = frameCountFunc->Call(obj, 0, args);
        Local<Number> frameCountNum = frameCountVal->ToNumber();
        
        cout << "Stack Trace:" << endl;
        
        int totalFrames = frameCountNum->Value();
        for(int i = 0; i < totalFrames; i++) {
            Local<Value> frameNumber[] = {NanNew<Number>(i)};
            Local<Value> setSelectedFrame = obj->Get(NanNew<String>("setSelectedFrame"));
            Local<Function> setSelectedFrameFunc = Local<Function>::Cast(setSelectedFrame);
            setSelectedFrameFunc->Call(obj, 1, frameNumber);
            
            Local<Value> frame = obj->Get(NanNew<String>("frame"));
            Local<Function> frameFunc = Local<Function>::Cast(frame);
            Local<Value> frameVal = frameFunc->Call(obj, 0, args);
            Local<Object> frameObj = frameVal->ToObject();
            Local<Value> frameToText = frameObj->Get(NanNew<String>("toText"));
            Local<Function> frameToTextFunc = Local<Function>::Cast(frameToText);
            Local<Value> frameToTextVal = frameToTextFunc->Call(frameObj, 0, args);
            String::Utf8Value frameText(frameToTextVal);
            cout << *frameText << endl;
        }
    } catch(exception  e) {
        cerr << "Error occurred while logging stack trace:" << e.what() << endl;
    }
    
}

#if (NODE_MODULE_VERSION > 0x000B)
static void DebugEventHandler2(const v8::Debug::EventDetails& event_details) {
    if (event_details.GetEvent() != v8::Break) return; // ignore other Debugger events from v8

    if (_show_backtrace) LogStackTrace(event_details.GetExecutionState());
}
#else
static void DebugEventHandler(DebugEvent event,
       Handle<Object> exec_state,
       Handle<Object> event_data,
       Handle<Value> data) {

    if (event != v8::Break) return;

    if (_show_backtrace) LogStackTrace(exec_state);
}
#endif

    
/** Will install/uninstall DebugEventListeners 
 * \param install true => install, false => uninstall
 *
 * Since this ends up making isolate-modifying calls to v8
 * it may be executed only from within the thread
 * that is executing the main v8 isolate.  In other words
 * it is *not* thread-safe/signal-safe.
 **/
static void InstallDebugEventListeners(bool install) {
    if (install) {
#if (NODE_MAJOR_VERSION > 0 || NODE_MINOR_VERSION >= 12 || (NODE_MINOR_VERSION >= 11 && NODE_PATCH_VERSION >= 15))
        v8::Debug::SetDebugEventListener(DebugEventHandler2);
#elif (NODE_MINOR_VERSION >= 11)
        v8::Debug::SetDebugEventListener2(DebugEventHandler2);
#else
        v8::Debug::SetDebugEventListener(DebugEventHandler);
#endif
    }
    else {
#if (NODE_MAJOR_VERSION > 0 || NODE_MINOR_VERSION >= 12 || (NODE_MINOR_VERSION >= 11 && NODE_PATCH_VERSION >= 15))
        v8::Debug::SetDebugEventListener(NULL);
#elif (NODE_MINOR_VERSION >= 11)
        v8::Debug::SetDebugEventListener2(NULL);
#else
        v8::Debug::SetDebugEventListener(NULL);
#endif
    }
}
    

static NAN_GETTER(GetterIPCMonitorPath) {
    NanScope();
    NanReturnValue(NanNew<String>(_ipcMonitorPath.c_str()));
}

static NAN_GETTER(GetterShowBackTrace) {
    NanScope();
    NanReturnValue(NanNew<Boolean>(_show_backtrace));
}

static NAN_SETTER(SetterShowBackTrace) {
    NanScope();
    bool newSetting = value->BooleanValue();
    if (newSetting != _show_backtrace) {
        _show_backtrace = newSetting;
        InstallDebugEventListeners(_show_backtrace);
    }
}

static NAN_METHOD(SetterIPCMonitorPath) {
    NanScope();
    if (args.Length() < 1 ||
        (!args[0]->IsString() && !args[0]->IsUndefined() && !args[0]->IsNull())) {
        THROW_BAD_ARGS();
    }
    String::Utf8Value ipcMonitorPath(args[0]);
    _ipcMonitorPath = *ipcMonitorPath;
    NanReturnValue(NanUndefined());
}

static NAN_METHOD(StartMonitor) {
    NanScope();
    NodeMonitor::getInstance().Start();
    NanReturnValue(NanUndefined());
}

static NAN_METHOD(StopMonitor) {
    NanScope();
    NodeMonitor::getInstance().Stop();
    NanReturnValue(NanUndefined());
}


static void SignalHangupActionHandler(int signo, siginfo_t* siginfo,  void* context) {
    savedSigInfo = *siginfo;
    hup_fired = 1;
    char c = 0;

    // enforce serial write via signal pipe
    // This will be handled by the monitor thread which is running a pselect() to
    // listen to any events on the other end of this pipe file descriptor pair.
    ssize_t rc = write(sigpipefd_w, &c, 1);  
    if (rc < 0) {
        perror("write");
    }
}

extern "C" void
init(Handle<Object> exports) {

    NODE_PROT_RO_PROPERTY(exports, "ipcMonitorPath", GetterIPCMonitorPath);
    NODE_PROTECTED_PROPERTY(exports, "showBacktrace", GetterShowBackTrace, SetterShowBackTrace);
    exports->Set(NanNew("setIpcMonitorPath"),
                 NanNew<FunctionTemplate>(SetterIPCMonitorPath)->GetFunction());
    exports->Set(NanNew("start"),
                 NanNew<FunctionTemplate>(StartMonitor)->GetFunction());
    exports->Set(NanNew("stop"),
                 NanNew<FunctionTemplate>(StopMonitor)->GetFunction());

    NodeMonitor::Initialize(v8::Isolate::GetCurrent());
    InstallDebugEventListeners(_show_backtrace);
    RegisterSignalHandler(SIGHUP, SignalHangupActionHandler);
    
}

NODE_MODULE(monitor, init)
}
