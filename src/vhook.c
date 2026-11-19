/*
 * vhook: LD_PRELOAD hook for the Anyka AK3918EV330 Tuya feeder (ak_tuya_ipc).
 *
 * Wraps ak_venc_encode_frame() from libmpi_venc.so. After the real encoder returns
 * a frame, it records per-channel statistics and, if VHOOK_CHN selects a channel,
 * copies that channel's raw H.264 (Annex B) to one TCP client on VHOOK_PORT.
 *
 * Signature confirmed by disassembling libmpi_venc V1.0.29:
 *   int ak_venc_encode_frame(int chn, const unsigned char *frame, unsigned int frame_len,
 *                            void *mdinfo, struct video_stream *stream);
 *   stream->data (offset 0): malloc'd copy of the encoded frame, freed by ak_venc_release_stream
 *   stream->len  (offset 4): its length; return 0 = success
 *   int ak_venc_request_idr(int chn);
 *
 * Rules this code keeps, because it runs inside the process that also drives feeding:
 *   - never blocks the encoder thread (non-blocking sockets, frames dropped instead)
 *   - never raises SIGPIPE (MSG_NOSIGNAL)
 *   - no threads, no extra libraries (only libc.so.0), tiny memory use
 *   - always calls the real encoder and returns its result unchanged
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct video_stream {
    unsigned char *data;
    unsigned int len;
};

typedef int (*encode_fn)(int, const unsigned char *, unsigned int, void *, struct video_stream *);
typedef int (*idr_fn)(int);

#define MAX_CHN 8
#define LOG_DIR "/tmp/vhook"      /* a directory: tuya_daemon's "rm" of files in /tmp leaves it */
#define LOG_MAX_LINES 300
#define PENDING_MAX (512 * 1024)  /* largest partial frame we will finish sending */

static encode_fn real_encode;
static idr_fn real_idr;
static int active;                /* 1 only inside /usr/sbin/ak_tuya_ipc */
static int cfg_chn = -1;          /* channel to stream; -1 = statistics only */
static int cfg_port = 8556;

static int lfd = -1, cfd = -1;
static time_t next_listen_try;
static int synced;                /* client has received an IDR since it connected */
static unsigned char *pending;    /* unsent tail of a partially sent frame */
static unsigned int pending_len, pending_off;

static unsigned long st_cnt[MAX_CHN], st_bytes[MAX_CHN], st_idr[MAX_CHN];
static unsigned int st_max[MAX_CHN];
static unsigned char st_first[MAX_CHN][16];   /* first bytes of the channel's first frame */
static unsigned char st_big[MAX_CHN][16];     /* first bytes of its largest frame */
static int st_codec[MAX_CHN];                 /* 4 = H.264, 5 = H.265, 0 = no keyframe yet */
static unsigned long st_sent, st_dropped, st_clients;
static time_t next_dump;
static int log_lines;

/*
 * Offline boot assist (specific to this ak_tuya_ipc build, firmware V68.01.10).
 *
 * After a boot without internet the main thread (fn 0x16c5c) waits forever at 0x16d94 for
 *   *(int *)0x1ba488 == 1      set by the net-status callback 0x1bdb8 when status 7 = "mqtt online"
 * and then, in 0x1a618, retries "get server time" until the SDK's time check 0x69740 passes, which needs
 *   *(int *)0x1F4998 != 0     clock synced: uni_time_set(utc, synced) 0x698c0 writes 1 at 0x6990c
 *   *(int *)0x1F4944 != 0     time zone received from the cloud: 0x69db0 writes 1 at 0x69e30
 * Until then the services after the wait (including the local control port 6668) never start.
 * If the cloud hasn't come online VHOOK_OFFLINE_DELAY seconds after start, we set both flags to the
 * values the firmware itself would write. The SDK then reports a 1970-based time, which the handler
 * treats as invalid: it tries NTP once (blocked, fails) and leaves the system clock alone.
 * The code at each spot is checked against this build first; on any mismatch the assist stays off.
 */
#define FW_MQTT_ONLINE  ((volatile int *)0x1ba488)
#define FW_TIME_SYNCED  ((volatile int *)0x1F4998)
#define FW_TZ_RECEIVED  ((volatile int *)0x1F4944)
static const struct { unsigned int addr, n, w[11]; } fw_sigs[] = {
    /* main thread: while (*0x1ba488 != 1) usleep(100000); then bl 0x1a618 */
    { 0x16d94, 8,  { 0xe59f6188, 0xe5965000, 0xe3550001, 0x0a000002, 0xe59f017c, 0xebfffc89, 0xeafffff9, 0xeb000e18 } },
    /* net-status callback, status 7: *0x1ba488 = 1 */
    { 0x1bdf0, 7,  { 0xe3a00006, 0xebfff9ca, 0xe59f0034, 0xebffe73c, 0xe59f3030, 0xe3a02001, 0xe5832000 } },
    /* time check: return (*0x1F4998 && *0x1F4944) ? 0 : -1 */
    { 0x69740, 11, { 0xe59f302c, 0xe08f3003, 0xe5933000, 0xe3530000, 0x0a000005, 0xe59f301c, 0xe79f0003,
                     0xe3500000, 0x13a00000, 0x03e00000, 0xe12fff1e } },
    /* uni_time_set: if (synced == 1) *0x1F4998 = synced */
    { 0x69900, 4,  { 0xe3570001, 0x059f3040, 0x008f3003, 0x05837000 } },
    /* time-zone parser: *0x1F4944 = 1 */
    { 0x69e20, 5,  { 0xe59f30c0, 0xe3a02001, 0xe08f3003, 0xe3a00000, 0xe5832000 } },
    /* OEM station-status function used by the SDK (f8318 -> 0x1b79c): if (!conn) { if (connect baidu.com:80 ok) conn = 1 } */
    { 0x1b7a4, 10, { 0xe92d4070, 0xe59f4098, 0xe1a05000, 0xe5d4336c, 0xe3530000, 0x1a000003, 0xeb04c271,
                     0xe3500000, 0x03a03001, 0x05c4336c } },
    /* ... if (conn == 1) { *out = 6 (station connected); return 0 } */
    { 0x1b804, 5,  { 0xe5d4336c, 0xe3530001, 0x1a000003, 0xe3a03006, 0xe5853000 } },
    /* lan_pro_cntl_init(bool): returns early if the LAN controller pointer at 0x1F5410 is already set */
    { 0x6fad8, 10, { 0xe92d4ff0, 0xe59f749c, 0xe59f949c, 0xe08f7007, 0xe5975000, 0xe08f9009, 0xe3550000,
                     0xe24dd07c, 0x0a00000d, 0xe59f2484 } },
    /* gw control struct getter: returns 0x1F737C */
    { 0x829ec, 4,  { 0xe59f0004, 0xe08f0000, 0xe12fff1e, 0x00174984 } },
    /* SDK log function: mgr = *(0x1F477C); drop if level > mgr->max_level (first int) */
    { 0x65bbc, 6,  { 0xe92d4ff0, 0xe59f8374, 0xe24dd04c, 0xe08f8008, 0xe598c000, 0xe35c0000 } },
    { 0x65c6c, 3,  { 0xe5993000, 0xe1560003, 0xcafffff6 } },
    /* get_gw_nw_status(): 0 invalid, 1 LAN valid, 2 cloud valid */
    { 0x83c4c, 10, { 0xe92d4010, 0xe24dd008, 0xe3a04000, 0xe28d0008, 0xe5604001, 0xebffff37, 0xe3500000,
                     0x11a00004, 0x1a000006, 0xe5dd3007 } },
    /* Wi-Fi status getter storing its result in the cache byte 0x1F7FC4 */
    { 0x83a18, 8,  { 0xe5d73000, 0xe3530006, 0x8a000033, 0xe5d64000, 0xe59f31ec, 0xe3a00000, 0xe08f3003,
                     0xe5c34000 } },
    /* SDK message post wrapper: post(msg_id, data, len) on the global queue */
    { 0x7b9c0, 10, { 0xe59fc020, 0xe52de004, 0xe08fc00c, 0xe1a0e001, 0xe1a03002, 0xe1a01000, 0xe1a0200e,
                     0xe59c0000, 0xe49de004, 0xea01eb22 } },
    /* LAN starter (calls lan_pro_cntl_init at 0x831ac), registered for message *(u16 *)0x1f7f98 */
    { 0x8301c, 9,  { 0xe92d41f0, 0xe59f3368, 0xe59f4368, 0xe59f5368, 0xe24dd010, 0xe08f3003, 0xe08f4004,
                     0xe08f5005, 0xe58d3004 } },
};
/*
 * Second stage. The SDK's local-control server (TCP 6668) is started by handler 0x8301c, which the SDK
 * runs when message *(u16 *)0x1f7f98 is posted. The subscription table at 0x1b5acc holds (key, handler)
 * pairs; the pair for this handler is 0x1b5afc = 0x1f7f98, 0x1b5b00 = 0x8301c. Offline,
 * nothing posts it, so 20 s after releasing the main thread we post it ourselves through the SDK's own
 * queue (0x7b9c0), unless 6668 is already listening.
 */
/*
 * The SDK's LAN server (6668) and discovery broadcast (6667) only run while get_gw_nw_status() (0x83c4c)
 * says the network is valid, which needs the OEM station status above to return 6. That only happens
 * after a TCP connect to baidu.com:80 succeeds (0x14c188), or after 200 polls with /tmp/resolv.conf
 * present; offline each poll blocks on the dropped connect, so it takes hours. The "connected" byte is
 * 0x1ba114 + 0x36c; we set it to 1 together with the other flags, as the firmware does after the check.
 */
#define FW_STA_CONNECTED ((volatile unsigned char *)0x1ba480)
#define FW_LAN_MSG_ID   ((volatile unsigned short *)0x1f7f98)
#define FW_POST_MSG     ((int (*)(unsigned int, void *, unsigned int))0x7b9c0)
static int cfg_offline = 1, cfg_offline_delay = 45, cfg_lan_kick = 0, cfg_lan_init = 0, cfg_sdk_log = -1;
static int offline_state;          /* 0 waiting, 2 flags set (LAN kick pending), 3 kicked, 1 done, -1 off */
static struct timespec t_start, t_stage;

static void vlog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int fd, n;
    if (log_lines >= LOG_MAX_LINES)
        return;
    log_lines++;
    n = snprintf(buf, sizeof(buf), "[%ld] ", (long)time(NULL));
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';
    fd = open(LOG_DIR "/log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (write(fd, buf, n) < 0) { /* nothing useful to do */ }
        close(fd);
    }
}

static void resolve(void)
{
    if (!real_encode)
        real_encode = (encode_fn)dlsym(RTLD_NEXT, "ak_venc_encode_frame");
    if (!real_idr)
        real_idr = (idr_fn)dlsym(RTLD_NEXT, "ak_venc_request_idr");
}

__attribute__((constructor)) static void vhook_init(void)
{
    char exe[64];
    const char *s;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

    /* Children started by ak_tuya_ipc (system(), popen()) must not load this library. */
    unsetenv("LD_PRELOAD");

    if (n <= 0)
        return;
    exe[n] = 0;
    if (strcmp(exe, "/usr/sbin/ak_tuya_ipc") != 0)
        return;

    if ((s = getenv("VHOOK_CHN")) != NULL)
        cfg_chn = atoi(s);
    if ((s = getenv("VHOOK_PORT")) != NULL && atoi(s) > 0)
        cfg_port = atoi(s);
    if (cfg_chn >= MAX_CHN)
        cfg_chn = -1;
    if ((s = getenv("VHOOK_OFFLINE")) != NULL)
        cfg_offline = atoi(s);
    if ((s = getenv("VHOOK_OFFLINE_DELAY")) != NULL && atoi(s) >= 20)
        cfg_offline_delay = atoi(s);
    if ((s = getenv("VHOOK_LAN_KICK")) != NULL)
        cfg_lan_kick = atoi(s);
    if ((s = getenv("VHOOK_LAN_INIT")) != NULL)
        cfg_lan_init = atoi(s);
    if ((s = getenv("VHOOK_SDK_LOG")) != NULL && atoi(s) >= 0 && atoi(s) <= 5)
        cfg_sdk_log = atoi(s);

    mkdir(LOG_DIR, 0755);
    resolve();
    active = 1;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    vlog("vhook loaded pid=%d chn=%d port=%d encode=%p idr=%p",
         (int)getpid(), cfg_chn, cfg_port, (void *)real_encode, (void *)real_idr);

    if (!cfg_offline) {
        offline_state = -1;
        vlog("offline assist: off (VHOOK_OFFLINE=0)");
    } else {
        unsigned int i, k;
        for (i = 0; i < sizeof(fw_sigs) / sizeof(fw_sigs[0]); i++)
            for (k = 0; k < fw_sigs[i].n; k++)
                if (((const unsigned int *)fw_sigs[i].addr)[k] != fw_sigs[i].w[k]) {
                    offline_state = -1;
                    vlog("offline assist: off, code at 0x%x differs from firmware V68.01.10", fw_sigs[i].addr + 4 * k);
                    return;
                }
        if (*(const unsigned int *)0x1b848 != 0x1ba114) {
            offline_state = -1;
            vlog("offline assist: off, station-status data pointer differs");
            return;
        }
        if (*(const unsigned int *)0x1b5afc != 0x1f7f98 || *(const unsigned int *)0x1b5b00 != 0x8301c) {
            cfg_lan_kick = 0;
            vlog("offline assist: LAN kick off, subscription table differs");
        }
        vlog("offline assist: armed, will release the main thread after %d s if the cloud is not online (LAN kick %s)",
             cfg_offline_delay, cfg_lan_kick ? "on" : "off");
    }
}

/* 1 if something listens on TCP 6668 (0x1A0C), from /proc/net/tcp. */
static int port_6668_listening(void)
{
    char buf[4096];
    int fd = open("/proc/net/tcp", O_RDONLY | O_CLOEXEC), n, found = 0;
    if (fd < 0)
        return 0;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        if (strstr(buf, ":1A0C 00000000:0000 0A")) {
            found = 1;
            break;
        }
    }
    close(fd);
    return found;
}

static long since(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - t->tv_sec);
}

/*
 * After the flags are set: watch for 6668 for up to 3 minutes, logging the SDK's own view every 10 s
 * (get_gw_nw_status 0x83c4c: 0 invalid / 1 LAN / 2 cloud; cached Wi-Fi status byte 0x1F7FC4). Posts the
 * LAN-start message once at 15 s only if VHOOK_LAN_KICK=1.
 */
#define FW_GET_NW_STATUS ((int (*)(void))0x83c4c)
#define FW_WIFI_CACHED   ((volatile unsigned char *)0x1F7FC4)
#define FW_LAN_CTRL      ((void * volatile *)0x1F5410)       /* lan_pro_cntl pointer, NULL until init */
#define FW_GW            ((volatile unsigned char *)0x1F737C) /* gw control struct */
#define FW_LAN_INIT      ((int (*)(int))0x6fad8)
#define FW_LOG_MGR       ((volatile int * volatile *)0x1F477C)  /* -> struct whose first int is the max level */
static int saved_log_level = -1;
static void offline_lan_kick(void)
{
    static char dummy = 0;
    static long last_check = -1;
    static int kicked, inited;
    long s = since(&t_stage);

    if (s == last_check || s % 2)      /* look at most once every 2 s */
        return;
    last_check = s;
    /* temporary SDK debug logging (VHOOK_SDK_LOG=level) for the first 60 s after release */
    if (cfg_sdk_log >= 0 && *FW_LOG_MGR) {
        if (saved_log_level < 0 && s < 60) {
            saved_log_level = **FW_LOG_MGR;
            **FW_LOG_MGR = cfg_sdk_log;
            vlog("offline assist: SDK log level %d -> %d for 60 s", saved_log_level, cfg_sdk_log);
        } else if (saved_log_level >= 0 && s >= 60) {
            **FW_LOG_MGR = saved_log_level;
            vlog("offline assist: SDK log level restored to %d", saved_log_level);
            cfg_sdk_log = -1;
        }
    }
    if (port_6668_listening()) {
        offline_state = 1;
        vlog("offline assist: 6668 LISTENING %ld s after release (nw=%d wifi=%d)", s, FW_GET_NW_STATUS(),
             *FW_WIFI_CACHED);
        return;
    }
    if (s % 10 == 0)
        vlog("offline assist: diag t+%ld nw=%d wifi=%d sta=%d online=%d lan=%p gw204=%d gw312=%d 6668=closed", s,
             FW_GET_NW_STATUS(), *FW_WIFI_CACHED, *FW_STA_CONNECTED, *FW_MQTT_ONLINE, *FW_LAN_CTRL,
             FW_GW[204], FW_GW[312]);
    if (cfg_lan_init && !inited && s >= 10) {
        inited = 1;
        if (*FW_LAN_CTRL == NULL) {
            int arg = FW_GW[204] == 5;
            vlog("offline assist: LAN controller missing, calling lan_pro_cntl_init(%d)", arg);
            vlog("offline assist: lan_pro_cntl_init returned %d, lan=%p", FW_LAN_INIT(arg), *FW_LAN_CTRL);
        } else {
            vlog("offline assist: LAN controller exists (%p), not calling init", *FW_LAN_CTRL);
        }
    }
    if (cfg_lan_kick && !kicked && s >= 15) {
        unsigned int id = *FW_LAN_MSG_ID;
        kicked = 1;
        if (id != 0xffff)
            vlog("offline assist: posted LAN-start message id %u, ret %d", id, FW_POST_MSG(id, &dummy, 1));
    }
    if (s >= 180) {
        offline_state = 1;
        vlog("offline assist: 6668 still closed 180 s after release");
    }
}

static void offline_assist(void)
{
    struct timespec now;
    long up;

    if (offline_state == 2 || offline_state == 3) {
        offline_lan_kick();
        return;
    }
    if (offline_state)
        return;
    if (*FW_MQTT_ONLINE == 1) {
        offline_state = 1;
        vlog("offline assist: not needed, cloud came online");
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    up = (long)(now.tv_sec - t_start.tv_sec);
    if (up < cfg_offline_delay)
        return;
    offline_state = 1;
    {
        /* At most 3 attempts per boot (/tmp is cleared on reboot), so a crash here can't loop forever. */
        char b[8] = {0};
        int n = 0, fd = open(LOG_DIR "/assist_count", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) {
            if (read(fd, b, sizeof(b) - 1) > 0)
                n = atoi(b);
            if (n >= 3) {
                close(fd);
                vlog("offline assist: already tried %d times this boot, giving up", n);
                return;
            }
            n = snprintf(b, sizeof(b), "%d\n", n + 1);
            if (lseek(fd, 0, SEEK_SET) == 0 && write(fd, b, n) < 0) { /* ignore */ }
            close(fd);
        }
    }
    vlog("offline assist: cloud not online after %ld s (synced=%d tz=%d sta=%d); setting flags", up,
         *FW_TIME_SYNCED, *FW_TZ_RECEIVED, *FW_STA_CONNECTED);
    *FW_STA_CONNECTED = 1;
    *FW_TIME_SYNCED = 1;
    *FW_TZ_RECEIVED = 1;
    *FW_MQTT_ONLINE = 1;
    vlog("offline assist: station-connected, clock-synced, time-zone and online flags set; main thread released");
    offline_state = 2;     /* then watch for 6668 (and post the LAN message only if VHOOK_LAN_KICK=1) */
    clock_gettime(CLOCK_MONOTONIC, &t_stage);
}

/* Returns 0 if not a keyframe, 4 for an H.264 keyframe, 5 for an H.265 keyframe.
 * H.265 NAL headers are two bytes with the second byte 0x01 (layer 0, temporal id 1);
 * types 32 VPS, 33 SPS, 34 PPS, 19/20 IDR. H.264 types 7 SPS and 5 IDR. */
static int is_idr(const unsigned char *d, unsigned int len)
{
    unsigned int i, lim = len < 256 ? len : 256;
    for (i = 0; i + 4 < lim; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            unsigned char b0 = d[i + 3], b1 = d[i + 4];
            int t5 = (b0 >> 1) & 0x3f, t4 = b0 & 0x1f;
            if (!(b0 & 0x80) && b1 == 0x01 && (t5 == 32 || t5 == 33 || t5 == 19 || t5 == 20))
                return 5;
            if (!(b0 & 0x80) && (t4 == 7 || t4 == 5))
                return 4;
        }
    }
    return 0;
}

static void drop_client(const char *why)
{
    if (cfd >= 0) {
        close(cfd);
        vlog("client closed: %s (sent=%lu dropped=%lu)", why, st_sent, st_dropped);
    }
    cfd = -1;
    synced = 0;
    free(pending);
    pending = NULL;
    pending_len = pending_off = 0;
}

static void open_listener(void)
{
    struct sockaddr_in a;
    int one = 1;
    time_t now = time(NULL);
    if (now < next_listen_try)
        return;
    next_listen_try = now + 5;

    lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        vlog("socket failed errno=%d", errno);
        return;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)cfg_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0) {
        vlog("bind/listen on %d failed errno=%d", cfg_port, errno);
        close(lfd);
        lfd = -1;
        return;
    }
    vlog("listening on tcp/%d for channel %d", cfg_port, cfg_chn);
}

/* Returns 1 if everything pending was sent, 0 if some is still waiting. */
static int flush_pending(void)
{
    while (pending && pending_off < pending_len) {
        ssize_t n = send(cfd, pending + pending_off, pending_len - pending_off,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            pending_off += n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            drop_client("send error");
            return 0;
        }
    }
    free(pending);
    pending = NULL;
    pending_len = pending_off = 0;
    return 1;
}

static void stream_frame(const unsigned char *d, unsigned int len)
{
    ssize_t n;

    if (lfd < 0)
        open_listener();
    if (lfd < 0)
        return;

    if (cfd < 0) {
        int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0)
            return;
        {
            int sz = 192 * 1024;
            setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
        }
        cfd = c;
        synced = 0;
        st_clients++;
        vlog("client connected (#%lu), requesting IDR", st_clients);
        if (real_idr)
            real_idr(cfg_chn);
    }

    if (pending && !flush_pending()) {
        /* Still busy with the previous frame: drop this one and resync at the next IDR. */
        if (cfd >= 0) {
            st_dropped++;
            synced = 0;
        }
        return;
    }
    if (cfd < 0)
        return;

    if (!synced) {
        if (!is_idr(d, len)) {
            st_dropped++;
            return;
        }
        synced = 1;
    }

    n = send(cfd, d, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == (ssize_t)len) {
        st_sent++;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        st_dropped++;          /* nothing went out: the stream is still clean */
        synced = 0;
    } else if (n < 0) {
        drop_client("send error");
    } else {
        /* Partial send: the rest must follow or the stream is corrupt. */
        unsigned int rest = len - (unsigned int)n;
        if (rest > PENDING_MAX || (pending = malloc(rest)) == NULL) {
            drop_client("frame too large to finish");
            return;
        }
        memcpy(pending, d + n, rest);
        pending_len = rest;
        pending_off = 0;
        st_sent++;
    }
}

static void dump_stats(void)
{
    char buf[1024];
    int i, n = 0, fd;
    time_t now = time(NULL);
    if (now < next_dump)
        return;
    next_dump = now + 10;
    n += snprintf(buf + n, sizeof(buf) - n, "time=%ld chn_cfg=%d port=%d listening=%d client=%d sent=%lu dropped=%lu clients=%lu\n",
                  (long)now, cfg_chn, cfg_port, lfd >= 0, cfd >= 0, st_sent, st_dropped, st_clients);
    for (i = 0; i < MAX_CHN && n < (int)sizeof(buf) - 200; i++) {
        int k;
        if (!st_cnt[i])
            continue;
        n += snprintf(buf + n, sizeof(buf) - n, "chn%d frames=%lu keyframes=%lu codec=h26%d bytes=%lu max=%u\n  first=",
                      i, st_cnt[i], st_idr[i], st_codec[i], st_bytes[i], st_max[i]);
        for (k = 0; k < 16; k++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02x", st_first[i][k]);
        n += snprintf(buf + n, sizeof(buf) - n, "\n  largest=");
        for (k = 0; k < 16; k++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02x", st_big[i][k]);
        n += snprintf(buf + n, sizeof(buf) - n, "\n");
    }
    fd = open(LOG_DIR "/stats.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (write(fd, buf, n) < 0) { /* ignore */ }
        close(fd);
        rename(LOG_DIR "/stats.tmp", LOG_DIR "/stats");
    }
}

int ak_venc_encode_frame(int chn, const unsigned char *frame, unsigned int frame_len,
                         void *mdinfo, struct video_stream *stream)
{
    int ret;

    if (!real_encode)
        resolve();
    if (!real_encode)
        return -1;   /* cannot happen if libmpi_venc.so is loaded */

    ret = real_encode(chn, frame, frame_len, mdinfo, stream);

    if (active && ret == 0 && stream && stream->data && stream->len &&
        chn >= 0 && chn < MAX_CHN) {
        unsigned int c = stream->len < 16 ? stream->len : 16;
        int key = is_idr(stream->data, stream->len);
        if (st_cnt[chn] == 0)
            memcpy(st_first[chn], stream->data, c);
        st_cnt[chn]++;
        st_bytes[chn] += stream->len;
        if (stream->len > st_max[chn]) {
            st_max[chn] = stream->len;
            memcpy(st_big[chn], stream->data, c);
        }
        if (key) {
            st_idr[chn]++;
            st_codec[chn] = key;
        }
        if (chn == cfg_chn)
            stream_frame(stream->data, stream->len);
        if (chn == 0)
            offline_assist();
        dump_stats();
    }
    return ret;
}
