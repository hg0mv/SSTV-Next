/* rig.c - radio control through Hamlib rigctld (taken over from DigiPan Next) */
#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <pthread.h>
#include <time.h>
#define MSG_NOSIGNAL 0
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <glob.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include "rig.h"

#define RIG_INT_PORT 14533          /* own rigctld; DigiPan uses 14532, so both can run */

int  cfg_model = 0, cfg_baud = 9600, cfg_cat = 0;
#ifdef _WIN32
char cfg_port[96] = "COM3"
#else
char cfg_port[96] = "/dev/ttyUSB0"
#endif
, cfg_model_name[80] = "";
int  rig_enabled = 0;
char rig_status[160] = "Rig: none (no PTT control / VOX)";
RigModel *rig_models = NULL;
int  rig_nmodels = 0;
char rig_ports[16][96];
int  rig_nports = 0;
const int rig_bauds[6] = { 4800, 9600, 19200, 38400, 57600, 115200 };

static char rig_host[64] = "127.0.0.1";
static int rig_port = 4532, rig_fd = -1, rig_models_loaded = 0;
#ifndef _WIN32
static pid_t rigd_pid = -1;
#endif
static pthread_mutex_t rig_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef _WIN32
static void hamlib_exe(const char *name, const char *env, char *out, size_t n);
static char *run_capture(const char *cmdline);
#endif
static void msleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

#ifdef _WIN32
static void rig_close_nl(void) { if (rig_fd >= 0) { closesocket((SOCKET)rig_fd); rig_fd = -1; } }
#else
static void rig_close_nl(void) { if (rig_fd >= 0) { close(rig_fd); rig_fd = -1; } }
#endif
static void rig_close(void) { pthread_mutex_lock(&rig_mutex); rig_close_nl(); pthread_mutex_unlock(&rig_mutex); }

#ifdef _WIN32
static int rig_connect(void)
{
    static int wsa;
    if (!wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1; wsa = 1; }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)rig_port);
    if (inet_pton(AF_INET, rig_host, &a.sin_addr) != 1) return -1;
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -1;
    u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(fd); return -1; }
        fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex); FD_SET(fd, &wr); FD_SET(fd, &ex);
        struct timeval tv = { 0, 800000 };
        if (select(0, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(fd, &ex)) { closesocket(fd); return -1; }
    }
    nb = 0; ioctlsocket(fd, FIONBIO, &nb);
    DWORD to = 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    return (int)fd;
}
#else
static int rig_connect(void)
{
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)rig_port);
    if (inet_pton(AF_INET, rig_host, &a.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        if (errno != EINPROGRESS) { close(fd); return -1; }
        struct pollfd pf; pf.fd = fd; pf.events = POLLOUT; pf.revents = 0;
        if (poll(&pf, 1, 800) <= 0) { close(fd); return -1; }
        int err = 0; socklen_t l = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fl);
    struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

#endif

/* One request/reply on the rigctld socket; one reconnect attempt. Caller holds rig_mutex. */
static int rig_xfer(const char *cmd, char *buf, size_t bsz)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (rig_fd < 0) rig_fd = rig_connect();
        if (rig_fd < 0) continue;
        if (send(rig_fd, cmd, strlen(cmd), MSG_NOSIGNAL) < 0) { rig_close_nl(); continue; }
        int n = (int)recv(rig_fd, buf, (int)bsz - 1, 0);
        if (n <= 0) { rig_close_nl(); continue; }
        buf[n] = 0;
        return (int)n;
    }
    return -1;
}

int rig_cmd(const char *cmd)
{
    char buf[128];
    pthread_mutex_lock(&rig_mutex);
    int n = rig_xfer(cmd, buf, sizeof buf);
    pthread_mutex_unlock(&rig_mutex);
    if (n < 0) return -1;
    return strstr(buf, "RPRT 0") ? 0 : -2;
}

int rig_ptt(int on) { return rig_enabled ? rig_cmd(on ? "T 1\n" : "T 0\n") : 0; }

int rig_get_freq(double *hz)
{
    if (!rig_enabled) return -1;
    char b[128];
    pthread_mutex_lock(&rig_mutex);
    int n = rig_xfer("f\n", b, sizeof b);
    pthread_mutex_unlock(&rig_mutex);
    if (n <= 0 || !strncmp(b, "RPRT", 4)) return -1;
    double f = atof(b);
    if (f < 1e5) return -1;
    *hz = f;
    return 0;
}

static void trim_copy(char *dst, size_t n, const char *s, size_t len)
{
    while (len > 0 && (*s == ' ')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
    if (len >= n) len = n - 1;
    memcpy(dst, s, len); dst[len] = 0;
}

/* Parse the output of "rigctl -l": fixed columns id(6) 2sp mfg(23) model(24) ... */
void rig_load_models(void)
{
    if (rig_models_loaded) return;
    rig_models_loaded = 1;
#ifdef _WIN32
    char exe[MAX_PATH + 32]; hamlib_exe("rigctl", "DIGIPAN_RIGCTL", exe, sizeof exe);
    char cmd[MAX_PATH + 64]; snprintf(cmd, sizeof cmd, "\"%s\" -l", exe);
    char *out = run_capture(cmd);
    if (!out) return;
    char *save = NULL;
#else
    const char *exe = getenv("DIGIPAN_RIGCTL"); if (!exe) exe = "rigctl";
    char cmd[300]; snprintf(cmd, sizeof cmd, "%s -l 2>/dev/null", exe);
    FILE *p = popen(cmd, "r"); if (!p) return;
#endif
    int cap = 0; char l[512];
#ifdef _WIN32
    for (char *ln = strtok_r(out, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        snprintf(l, sizeof l, "%s", ln);
#else
    while (fgets(l, sizeof l, p)) {
#endif
        int id = atoi(l); size_t L = strlen(l);
        if (id <= 0 || L < 32 || l[6] != ' ') continue;
        char mfg[32], mod[40];
        trim_copy(mfg, sizeof mfg, l + 8, 23);
        trim_copy(mod, sizeof mod, l + 31, L - 31 > 24 ? 24 : L - 31);
        if (rig_nmodels == cap) {
            cap = cap ? cap * 2 : 512;
            RigModel *nm = realloc(rig_models, (size_t)cap * sizeof *nm);
            if (!nm) break;
            rig_models = nm;
        }
        rig_models[rig_nmodels].id = id;
        snprintf(rig_models[rig_nmodels].name, sizeof rig_models[0].name, "%s %s", mfg, mod);
        rig_nmodels++;
    }
#ifdef _WIN32
    free(out);
#else
    pclose(p);
#endif
}

#ifdef _WIN32
/* serial ports from HKLM\HARDWARE\DEVICEMAP\SERIALCOMM ("COM3", ...) */
void rig_scan_ports(void)
{
    rig_nports = 0;
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0; rig_nports < 16; i++) {
        char name[256], val[96]; DWORD nl = sizeof name, vl = sizeof val, type;
        if (RegEnumValueA(k, i, name, &nl, NULL, &type, (BYTE *)val, &vl) != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        val[vl < sizeof val ? vl : sizeof val - 1] = 0;
        snprintf(rig_ports[rig_nports++], sizeof rig_ports[0], "%.90s", val);
    }
    RegCloseKey(k);
}
#else
void rig_scan_ports(void)
{
    const char *pats[] = { "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/serial/by-id/*" };
    rig_nports = 0;
    for (int k = 0; k < 3; k++) {
        glob_t g; if (glob(pats[k], 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc && rig_nports < 16; i++)
            snprintf(rig_ports[rig_nports++], sizeof rig_ports[0], "%.90s", g.gl_pathv[i]);
        globfree(&g);
    }
}

#endif

#ifdef _WIN32
static HANDLE rigd_proc, rigd_job;

/* Hamlib program: env override, else hamlib\ next to our exe, else the PATH */
static void hamlib_exe(const char *name, const char *env, char *out, size_t n)
{
    const char *e = getenv(env);
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    char me[MAX_PATH];
    DWORD l = GetModuleFileNameA(NULL, me, sizeof me);
    if (l > 0 && l < sizeof me) {
        char *sl = strrchr(me, '\\'); if (sl) *sl = 0;
        snprintf(out, n, "%s\\hamlib\\%s.exe", me, name);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return;
    }
    snprintf(out, n, "%s.exe", name);
}

/* runs a console program without a window and returns its output (malloc'd) */
static char *run_capture(const char *cmdline)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    char cl[1024]; snprintf(cl, sizeof cl, "%s", cmdline);
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return NULL; }
    CloseHandle(wr);
    size_t cap = 65536, n = 0; char *buf = malloc(cap);
    DWORD got;
    while (ReadFile(rd, buf + n, (DWORD)(cap - n - 1), &got, NULL) && got > 0) {
        n += got;
        if (cap - n < 4096) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[n] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return buf;
}

static void rigd_stop(void)
{
    rig_close();
    if (rigd_proc) {
        TerminateProcess(rigd_proc, 0);
        WaitForSingleObject(rigd_proc, 1500);
        CloseHandle(rigd_proc);
        rigd_proc = NULL;
    }
}

static int rigd_start(void)
{
    char exe[MAX_PATH + 32]; hamlib_exe("rigctld", "DIGIPAN_RIGCTLD", exe, sizeof exe);
    /* COM10 and above need the \\.\ prefix */
    char port[120];
    if (!_strnicmp(cfg_port, "COM", 3) && atoi(cfg_port + 3) >= 10) snprintf(port, sizeof port, "\\\\.\\%s", cfg_port);
    else snprintf(port, sizeof port, "%s", cfg_port);
    char cl[1024];
    snprintf(cl, sizeof cl, "\"%s\" -m %d -r %s -s %d -t %d -T 127.0.0.1", exe, cfg_model, port, cfg_baud, RIG_INT_PORT);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { snprintf(rig_status, sizeof rig_status, "Rig: cannot create pipe"); return -1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdError = wr; si.hStdOutput = NULL; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        snprintf(rig_status, sizeof rig_status, "Rig: rigctld.exe not found - put Hamlib into the hamlib folder");
        return -1;
    }
    /* the job kills rigctld when we exit, even after a crash: never a stray rigctld keeping the port */
    if (!rigd_job) {
        rigd_job = CreateJobObjectA(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (rigd_job) SetInformationJobObject(rigd_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (rigd_job) AssignProcessToJobObject(rigd_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    rigd_proc = pi.hProcess;
    int fd = -1;
    for (int i = 0; i < 60; i++) {
        if (WaitForSingleObject(rigd_proc, 0) == WAIT_OBJECT_0) {
            char e[200] = ""; DWORD avail = 0, got = 0;
            if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) { ReadFile(rd, e, sizeof e - 1, &got, NULL); e[got] = 0; }
            for (char *q2 = e; *q2; q2++) if (*q2 == '\n' || *q2 == '\r') { *q2 = 0; break; }
            snprintf(rig_status, sizeof rig_status, "Rig: rigctld failed: %.110s", e[0] ? e : "check port/model/baud");
            CloseHandle(rigd_proc); rigd_proc = NULL; CloseHandle(rd);
            return -1;
        }
        fd = rig_connect();
        if (fd >= 0) break;
        msleep_ms(100);
    }
    CloseHandle(rd);
    if (fd < 0) { rigd_stop(); snprintf(rig_status, sizeof rig_status, "Rig: rigctld did not start listening"); return -1; }
    pthread_mutex_lock(&rig_mutex); rig_close_nl(); rig_fd = fd; pthread_mutex_unlock(&rig_mutex);
    return 0;
}
#else
static void rigd_stop(void)
{
    rig_close();
    if (rigd_pid > 0) {
        kill(rigd_pid, SIGTERM);
        int done = 0;
        for (int i = 0; i < 30 && !done; i++) { if (waitpid(rigd_pid, NULL, WNOHANG) == rigd_pid) done = 1; else msleep_ms(50); }
        if (!done) { kill(rigd_pid, SIGKILL); waitpid(rigd_pid, NULL, 0); }
        rigd_pid = -1;
    }
}

/* Start rigctld as a child and wait until it listens. 0 = ok, else message in rig_status. */
static int rigd_start(void)
{
    const char *exe = getenv("DIGIPAN_RIGCTLD"); if (!exe) exe = "rigctld";
    char m[16], b[16], t[16];
    snprintf(m, sizeof m, "%d", cfg_model); snprintf(b, sizeof b, "%d", cfg_baud); snprintf(t, sizeof t, "%d", RIG_INT_PORT);
    int ep[2]; if (pipe(ep) != 0) { snprintf(rig_status, sizeof rig_status, "Rig: cannot create pipe"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(ep[0]); close(ep[1]); snprintf(rig_status, sizeof rig_status, "Rig: fork failed"); return -1; }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);              /* die with us, never leave a stray rigctld */
        if (getppid() == 1) _exit(1);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); }
        dup2(ep[1], 2); close(ep[0]); close(ep[1]);
        execlp(exe, exe, "-m", m, "-r", cfg_port, "-s", b, "-t", t, "-T", "127.0.0.1", (char *)NULL);
        dprintf(2, "cannot run %s", exe); _exit(127);
    }
    close(ep[1]); fcntl(ep[0], F_SETFL, fcntl(ep[0], F_GETFL, 0) | O_NONBLOCK);
    rigd_pid = pid;
    int fd = -1;
    for (int i = 0; i < 60; i++) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            rigd_pid = -1;
            char e[200] = "";
            ssize_t n = read(ep[0], e, sizeof e - 1); if (n < 0) n = 0; e[n] = 0;
            for (char *q = e; *q; q++) if (*q == '\n' || *q == '\r') { *q = 0; break; }
            if (WIFEXITED(st) && WEXITSTATUS(st) == 127) snprintf(rig_status, sizeof rig_status, "Rig: rigctld not found - install Hamlib");
            else snprintf(rig_status, sizeof rig_status, "Rig: rigctld failed: %.110s", e[0] ? e : "check port/model/baud");
            close(ep[0]); return -1;
        }
        fd = rig_connect();
        if (fd >= 0) break;
        msleep_ms(100);
    }
    close(ep[0]);
    if (fd < 0) { rigd_stop(); snprintf(rig_status, sizeof rig_status, "Rig: rigctld did not start listening"); return -1; }
    pthread_mutex_lock(&rig_mutex); rig_close_nl(); rig_fd = fd; pthread_mutex_unlock(&rig_mutex);
    return 0;
}

#endif

void rig_use_external(const char *host, int port)
{
    rigd_stop();
    snprintf(rig_host, sizeof rig_host, "%.60s", host);
    rig_port = port;
    rig_enabled = 1;
    snprintf(rig_status, sizeof rig_status, "Rig: external rigctld %s:%d", rig_host, rig_port);
}

int rig_apply(void)
{
    rigd_stop();
    if (!cfg_cat) { rig_enabled = 0; snprintf(rig_status, sizeof rig_status, "Rig: PTT control off (VOX)"); return 0; }
    if (cfg_model <= 0) { rig_enabled = 0; snprintf(rig_status, sizeof rig_status, "Select a radio model first"); return -1; }
    snprintf(rig_status, sizeof rig_status, "Rig: starting %.40s ...", cfg_model_name);
    snprintf(rig_host, sizeof rig_host, "127.0.0.1"); rig_port = RIG_INT_PORT;
    if (rigd_start() != 0) { rig_enabled = 0; return -1; }
    rig_enabled = 1;
    double hz;
    if (rig_get_freq(&hz) == 0) snprintf(rig_status, sizeof rig_status, "Rig: %.40s - CAT OK, %.4f MHz", cfg_model_name, hz / 1e6);
    else snprintf(rig_status, sizeof rig_status, "Rig: %.40s - rigctld runs but the radio does not answer (check cable/baud/CAT menu)", cfg_model_name);
    return 0;
}

void rig_shutdown(void)
{
    rig_ptt(0);                     /* belt and braces */
    rig_close();
    rigd_stop();
}
