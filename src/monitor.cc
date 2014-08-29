/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */

#include <sys/syscall.h>

#include <sys/types.h>
#include <sys/select.h>
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
static bool _show_backtrace = true;
static const int MAX_INACTIVITY_RETRIES = 5;
// default is to show a backtrace when receiving a HUP
static const int REPORT_INTERVAL_MS = 1000;

/* globals used for signal catching, etc */
static volatile sig_atomic_t hup_fired = 0;
static siginfo_t savedSigInfo;
static sigset_t savedBlockSet;
static int sigpipefd_w = -1;
static int sigpipefd_r = -1;


namespace node {

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

// sleep by using select
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
    if (res >= 0 && FD_ISSET(sigpipefd_r, &readfds)) {
        char c[100];
        while ((read(sigpipefd_r, &c, sizeof(c))) == sizeof(c)) {
            /* Flush the pipe! */
        }
    }
    else {
        perror("pselect");
    }
}

NodeMonitor* NodeMonitor::instance_ = NULL;

/*
 * Thread which reports the
 * status of the current process to the watcher
 * process.
 */
void* monitorNodeThread(void *arg) {
    int errorCounter = 0;

    doSleep(REPORT_INTERVAL_MS);
    while (true) {
        if (hup_fired) {
            // We can call DebugBreak from outside the main v8 thread, and the v8 engine
            // will try to make a debugger callback when it next checks a StackGuard
            // (usually at entry/exit of functions that are in the process of being optimized)
            siginfo_t *siginfo = &savedSigInfo;
            std::cout << "Process " << getpid() << " received SIGHUP from Process (pid: "
                      << siginfo->si_pid << " uid: " << siginfo->si_uid << ")" << std::endl;
            v8::Debug::DebugBreak();
            hup_fired = 0;
        }
        if (!errorCounter) {
            if (!NodeMonitor::sendReport()) {
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

// Node 0.11+
#if (NODE_MODULE_VERSION > 0x000B)
void updateLoopTimeStamp(uv_async_t *watcher) {
    NodeMonitor::setStatistics();
}
#else
void updateLoopTimeStamp(uv_async_t *watcher, int revents = 0) {
    NodeMonitor::setStatistics();
}
#endif

/*
 * NodeMonitor
 * Spawns a thread to monitor stats every REPORT_INTERVAL_MS
 */
void NodeMonitor::Initialize() {

    // only one instance is allowed per process
    if (instance_) {
        return;
    }
    instance_ = new NodeMonitor();

    instance_->ipcSocket_ = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (instance_->ipcSocket_ != -1) {
        fcntl(instance_->ipcSocket_, F_SETFD, FD_CLOEXEC);
    }

    /* Use a pipe to let the signal handler (which may be in another thread) break out of pselect() */
    {
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


    uv_async_init(uv_default_loop(), &instance_->check_loop_, updateLoopTimeStamp);
    uv_unref((uv_handle_t*)&instance_->check_loop_);

    ipcInitialization();
    pthread_create(&instance_->tmonitor_, NULL, monitorNodeThread, NULL);
}

void NodeMonitor::setStatistics() {

    instance_->pending_ = 0;
    instance_->loop_timestamp_ = uv_hrtime();
    instance_->loop_count_++;

    // obtain memory ration
    v8::HeapStatistics v8stats;
#if (NODE_MODULE_VERSION > 0x000B)
    NanGetHeapStatistics(&v8stats);
#else
    V8::GetHeapStatistics(&v8stats);
#endif
    double pmem = (v8stats.used_heap_size() / (double) v8stats.total_heap_size());

    // Obtains the CPU usage
    float scpu = 0.0;
    float ucpu = 0.0;
    long int uticks = 0;
    long int sticks = 0;

    instance_->cpuTrackerSync_.GetCurrent(&ucpu, &scpu, &uticks, &sticks);

    // Get current number of requests
    unsigned int currReqs = getIntFunction("getTotalRequestCount");
    unsigned int reqDelta = currReqs - instance_->stats_.lastRequests_;

    // Get the current time
    struct timeval cur_time = { 0, 0 };
    gettimeofday(&cur_time, NULL);

    // milliseconds
    long timeDelta = (cur_time.tv_sec * 1000 + cur_time.tv_usec / 1000)
        - (instance_->stats_.lastTime_.tv_sec * 1000 + instance_->stats_.lastTime_.tv_usec / 1000);

    // Update the number of requests processed
    // and the ratio CPU/req.
    instance_->stats_.lastRequests_ = currReqs;
    instance_->stats_.lastCpuPerReq_ = (reqDelta <= 0) ? 0 : (scpu + ucpu) / reqDelta;
    instance_->stats_.lastJiffiesPerReq_ = (reqDelta <= 0) ? 0
        : ((float) (sticks + uticks)) / reqDelta;

    // Request delta - requests since last check.
    instance_->stats_.lastReqDelta_ = reqDelta;
    instance_->stats_.timeDelta_ = timeDelta;

    // Update time
    instance_->stats_.lastTime_.tv_sec = cur_time.tv_sec;
    instance_->stats_.lastTime_.tv_usec = cur_time.tv_usec;

    // Last RPS
    instance_->stats_.lastRPS_ = (int) (reqDelta / (((double) timeDelta) / 1000));

    // Get currently open requests
    instance_->stats_.currentOpenReqs_ = getIntFunction("getRequestCount");

    // currently open connections
    instance_->stats_.currentOpenConns_ = getIntFunction("getOpenConnections");

    // Kb of transferred data
    float dataTransferred = ((float) (getIntFunction("getTransferred"))) / 1024;

    instance_->stats_.lastKBytesSecond = (dataTransferred - instance_->stats_.lastKBytesTransfered_) / (((double) timeDelta) / 1000);
    instance_->stats_.lastKBytesTransfered_ = dataTransferred;
    instance_->stats_.healthIsDown_ = getBooleanFunction("isDown");
    instance_->stats_.healthStatusCode_ = getIntFunction("getStatusCode");
    instance_->stats_.healthStatusTimestamp_ = (time_t) getIntFunction("getStatusTimestamp");

    instance_->stats_.pmem_ = pmem;
}

void NodeMonitor::ipcInitialization() {
    memset(&instance_->ipcAddr_, 0, sizeof(instance_->ipcAddr_));
    instance_->ipcAddr_.sun_family = AF_UNIX;

    strncpy(instance_->ipcAddr_.sun_path, _ipcMonitorPath.c_str(),
        sizeof(instance_->ipcAddr_.sun_path));
    instance_->ipcAddrLen_ = sizeof(instance_->ipcAddr_.sun_family) + strlen(instance_->ipcAddr_.sun_path) + 1;

    memset(&instance_->msg_, 0, sizeof(instance_->msg_));

    instance_->msg_.msg_name = &instance_->ipcAddr_;
    instance_->msg_.msg_namelen = instance_->ipcAddrLen_;
    instance_->msg_.msg_iovlen = 1;
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

bool NodeMonitor::sendReport() {
    static pid_t pid = getpid();
    static double minOverHead = 0;

    // See how many reports has been processed since last call to this function
    unsigned int diff_count = instance_->loop_count_ - instance_->last_loop_count_ + 1;
    instance_->last_loop_count_ = instance_->loop_count_;

    // The different between current and previous time (in miliseconds)
    double ts_diff = (instance_->loop_timestamp_ - instance_->start_timestamp_) / 1.0e6;

    // Obtains the CPU usage
    float scpu = 0.0f;
    float ucpu = 0.0f;
    float cpusum = 0.0f;
    long int uticks = 0;
    long int sticks = 0;

    instance_->cpuTracker_.GetCurrent(&ucpu, &scpu, &uticks, &sticks);

    cpusum = ucpu + scpu;

    // Obtain the time elapsed since last event
    if (ts_diff > 0) {
        if (minOverHead == 0.0 || minOverHead > ts_diff) {
            minOverHead = ts_diff;
        }
    } else {
    ts_diff = -ts_diff;
    }

    instance_->consumption_ = cpusum;
    Statistics& stats = instance_->stats_;

    char buffer[50];

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

    snprintf(buffer, sizeof(buffer), "\"ts\":%.2f,", instance_->loop_timestamp_ / 1.0e6);
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
    snprintf(buffer, sizeof(buffer), "\"utcstart\":%d,", (int) instance_->startTime);
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
    data.erase(data.size() - 1);; //get rid of last comma
    
    data.append("}}");

    // Send datagram notification to the listener
    // If Any
    struct iovec vec;

    vec.iov_base = (void *) data.c_str();
    vec.iov_len = strlen((char *) vec.iov_base);
    instance_->msg_.msg_iov = &vec;
    int rc = sendmsg(instance_->ipcSocket_, &instance_->msg_, MSG_DONTWAIT);

    if (!instance_->pending_) {
        instance_->start_timestamp_ = uv_hrtime();
        instance_->pending_ = 1;
    }
    uv_async_send(&instance_->check_loop_);
    return (rc != -1);
}

void NodeMonitor::Stop() {
    if (instance_ == NULL) {
        return;
    }
    pthread_cancel(instance_->tmonitor_);
    close(instance_->ipcSocket_);
    delete instance_;
    instance_ = NULL;
}

NodeMonitor::~NodeMonitor() {
}

NodeMonitor::NodeMonitor() :
    startTime(0),
    tmonitor_((pthread_t) NULL),
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
    _show_backtrace = value->BooleanValue();
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
    NodeMonitor::Initialize();
    NanReturnValue(NanUndefined());
}

static NAN_METHOD(StopMonitor) {
    NanScope();
    NodeMonitor::Stop();
    NanReturnValue(NanUndefined());
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

static void SignalHangupActionHandler(int signo, siginfo_t* siginfo,  void* context) {
    savedSigInfo = *siginfo;
    hup_fired = 1;
    char c = 0;
    write(sigpipefd_w, &c, 1);  // signal via pipe fd to cause pselect() calling thread to break
}


extern "C" void
init(Handle<Object> exports) {

    NODE_PROT_RO_PROPERTY(exports, "ipcMonitorPath", GetterIPCMonitorPath);
    NODE_PROTECTED_PROPERTY(exports, "backtrace", GetterShowBackTrace, SetterShowBackTrace);
    exports->Set(NanNew("setIpcMonitorPath"),
                 NanNew<FunctionTemplate>(SetterIPCMonitorPath)->GetFunction());
    exports->Set(NanNew("start"),
                 NanNew<FunctionTemplate>(StartMonitor)->GetFunction());
    exports->Set(NanNew("stop"),
                 NanNew<FunctionTemplate>(StopMonitor)->GetFunction());

// Always install our debugger event listener as soon as we're initialized.
// We can't install it once we're running in a different thread if the main v8 
// thread is busy or hung
#if (NODE_MODULE_VERSION > 0x000B)
// Node 0.11+
    v8::Debug::SetDebugEventListener2(DebugEventHandler2);
#else
    v8::Debug::SetDebugEventListener(DebugEventHandler);
#endif

    RegisterSignalHandler(SIGHUP, SignalHangupActionHandler);
}

NODE_MODULE(monitor, init)
}
