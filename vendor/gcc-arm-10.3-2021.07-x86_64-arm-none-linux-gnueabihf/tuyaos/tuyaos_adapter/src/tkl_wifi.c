/**
 * @file tkl_wifi.c
 * @brief TuyaOS Wi-Fi TKL for ATK-DLRK3506B (RTL8733BU + wpa_supplicant/hostapd)
 * @note SoftAP uses wlan1, STA uses wlan0 — same as board wifi_apmode.sh.
 *       Do NOT run SoftAP on wlan0: AP→STA handoff on one iface breaks RTL8733BU
 *       (associate OK, DHCP/IP never comes, linkage not ready).
 * @copyright Copyright (c) 2024 Tuya Inc. All Rights Reserved.
 */
#include "tuya_cloud_types.h"
#include "tkl_wifi.h"
#include "tkl_init_wifi.h"
#include "tkl_thread.h"
#include "tkl_memory.h"
#include "tkl_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/wireless.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define TKL_WLAN_IFNAME      "wlan0"
#define TKL_WLAN_AP_IFNAME   "wlan1"
#define TKL_WPA_CLI          "wpa_cli -i " TKL_WLAN_IFNAME " "
#define TKL_HOSTAPD_CONF     "/tmp/tkl_hostapd.conf"
#define TKL_DNSMASQ_CONF     "/tmp/tkl_dnsmasq.conf"
#define TKL_HOSTAPD_PID      "/tmp/tkl_hostapd.pid"
#define TKL_WPA_CONF         "/tmp/tkl_wpa_supplicant.conf"
#define TKL_DEF_AP_IP        "192.168.176.1"
#define TKL_DEF_AP_MASK      "255.255.255.0"
#define TKL_DEF_AP_CHAN      6
#define TKL_DEF_AP_MAX_CONN  8
#define TKL_DEF_AP_BEACON    100
#define TKL_CONNECT_FAIL_SEC 60
/* Don't hammer wpa with scan+reassociate every second while it is stuck. */
#define TKL_REASSOC_MIN_SEC  5
/* Throttle for the "wait IP" progress trace. */
#define TKL_PROGRESS_LOG_SEC 3

#define TKL_WIFI_LOGFILE     "/tmp/tkl_wifi.log"

/* Extra on-device probes (ping/openssl). Off by default — see __sta_post_got_ip. */
#ifndef TKL_WIFI_DEBUG
#define TKL_WIFI_DEBUG 0
#endif

/*
 * The monitor thread logs once a second forever, so the old macro's
 * fopen+fprintf+fclose per line was a permanent syscall tax. Keep one handle
 * open instead (opened lazily, line-buffered so a crash still leaves the tail).
 */
static FILE *s_logf = NULL;

static VOID_T tkl_log_write(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (s_logf == NULL) {
        s_logf = fopen(TKL_WIFI_LOGFILE, "a");
        if (s_logf != NULL) {
            setvbuf(s_logf, NULL, _IOLBF, 0);
        }
    }
    if (s_logf != NULL) {
        va_start(ap, fmt);
        vfprintf(s_logf, fmt, ap);
        va_end(ap);
    }
}

#define TKL_LOG(fmt, ...) tkl_log_write("[tkl_wifi] " fmt "\n", ##__VA_ARGS__)

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
static WIFI_EVENT_CB     s_wifi_cb    = NULL;
static WF_WK_MD_E        s_wifi_mode  = WWM_STATION;
static pthread_t         s_mon_tid;
static BOOL_T            s_mon_run    = FALSE;
static WF_STATION_STAT_E s_last_stat  = WSS_IDLE;
static CHAR_T            s_conn_ssid[WIFI_SSID_LEN + 1] = {0};
static NW_IP_S           s_ap_ip;
static BOOL_T            s_ap_ip_valid = FALSE;
static time_t            s_conn_start_ts = 0;
static BOOL_T            s_conn_fail_reported = FALSE;
static time_t            s_dhcp_last_try = 0;
static time_t            s_reassoc_last_try = 0;

/**
 * @brief Station status enum -> short name, so logs read as state transitions
 *        instead of bare integers.
 */
static const char *__stat_name(WF_STATION_STAT_E s)
{
    switch (s) {
    case WSS_IDLE:            return "IDLE";
    case WSS_CONNECTING:      return "CONNECTING";
    case WSS_PASSWD_WRONG:    return "PASSWD_WRONG";
    case WSS_NO_AP_FOUND:     return "NO_AP_FOUND";
    case WSS_CONN_FAIL:       return "CONN_FAIL";
    case WSS_CONN_SUCCESS:    return "CONN_SUCCESS";
    case WSS_GOT_IP:          return "GOT_IP";
    case WSS_DHCP_FAIL:       return "DHCP_FAIL";
    default:                  return "?";
    }
}

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
static VOID_T __wifi_takeover_iface(VOID_T);
static VOID_T __wifi_flush_iface(CONST CHAR_T *ifn);
static VOID_T __ensure_ap_iface(VOID_T);
static VOID_T __sta_dhcp_ensure(BOOL_T force);
static VOID_T __sta_post_got_ip(VOID_T);
static VOID_T __sta_force_idle(VOID_T);
static VOID_T __wpa_write_empty_conf(VOID_T);
static VOID_T tkl_wpa_restart(VOID_T);
static VOID_T tkl_hex_quote(char *dst, int dstsz, const UCHAR_T *src, int srclen);
static VOID_T __sta_recover_after_ap(VOID_T);
static OPERATE_RET __sta_connect_via_conf(CONST CHAR_T *ssid, CONST CHAR_T *passwd);
static BOOL_T __is_ap_ip(CONST CHAR_T *ip);
static BOOL_T __ssid_is_expected(CONST CHAR_T *st);
static OPERATE_RET __resolve_ap_ip(CONST WF_AP_CFG_IF_S *cfg, NW_IP_S *out);
OPERATE_RET tkl_wifi_stop_ap(VOID_T);
OPERATE_RET tkl_wifi_station_disconnect(VOID_T);
OPERATE_RET tkl_wifi_get_ip(CONST WF_IF_E wf, NW_IP_S *ip);
OPERATE_RET tkl_wifi_station_get_status(WF_STATION_STAT_E *stat);

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
static int tkl_shell(const char *cmd, char *out, int outsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    int n = 0;
    if (out && outsz > 0) {
        out[0] = 0;
        n = (int)fread(out, 1, (size_t)outsz - 1, fp);
        if (n < 0) {
            n = 0;
        }
        out[n] = 0;
    } else {
        char buf[256];
        while (fread(buf, 1, sizeof(buf), fp) > 0) {
            /* drain */
        }
    }
    pclose(fp);
    return n;
}

/**
 * @brief Parse key=value from wpa_cli status into a caller buffer (line-anchored)
 * @param[in] text status text
 * @param[in] key field name
 * @param[out] val destination, always NUL-terminated when the function returns
 * @param[in] valsz destination size
 * @return TRUE when the key was found
 * @note Must match at line start only — a naive strstr("ssid=") also hits
 *       inside "bssid=".
 * @note This used to return a pointer into a single static buffer, so any two
 *       live results aliased each other. tkl_stat_from_status held the
 *       "wpa_state" result across __ssid_is_expected(), which re-entered this
 *       function for "ssid" and silently rewrote it underneath. It only
 *       survived because every COMPLETED branch happened to return early.
 */
static BOOL_T tkl_kv(const char *text, const char *key, char *val, size_t valsz)
{
    const char *p;
    size_t klen;
    size_t i;

    if (!val || valsz == 0) {
        return FALSE;
    }
    val[0] = '\0';
    if (!text || !key || key[0] == '\0') {
        return FALSE;
    }

    klen = strlen(key);
    p = text;
    while (p && *p) {
        if ((p == text || *(p - 1) == '\n' || *(p - 1) == '\r') &&
            strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < valsz - 1) {
                val[i++] = *p++;
            }
            val[i] = '\0';
            return TRUE;
        }
        p = strchr(p, '\n');
        if (p) {
            p++;
        }
    }
    return FALSE;
}

static int tkl_wpa(const char *args, char *out, int outsz)
{
    char cmd[320];
    /* timeout avoids wedging SDK thread when wpa/driver hangs after SoftAP. */
    snprintf(cmd, sizeof(cmd), "timeout 2 " TKL_WPA_CLI "%s 2>/dev/null", args);
    return tkl_shell(cmd, out, outsz);
}

static VOID_T tkl_wifi_rfkill_unblock(VOID_T)
{
    tkl_shell("rfkill unblock wifi 2>/dev/null", NULL, 0);
}

/**
 * @brief Wait until a process appears, polling instead of sleeping blindly.
 * @param[in] name process name for pidof
 * @param[in] max_ms overall budget in milliseconds
 * @return elapsed ms on success, -1 on timeout
 * @note SoftAP start-up sat squarely in front of BLE advertising: the SDK
 *       brings the hotspot fully up before it touches BLE at all, so every
 *       fixed usleep here delayed the moment the phone could see the device.
 *       Polling costs one cheap pidof per 50ms and normally returns far sooner
 *       than the old worst-case sleep.
 */
static BOOL_T __proc_running(CONST CHAR_T *name)
{
    CHAR_T cmd[64] = {0};
    CHAR_T out[64] = {0};

    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", name);
    return (tkl_shell(cmd, out, sizeof(out)) > 0 && out[0] != '\0') ? TRUE : FALSE;
}

static INT_T __wait_proc(CONST CHAR_T *name, INT_T max_ms)
{
    INT_T waited = 0;

    while (waited <= max_ms) {
        if (__proc_running(name)) {
            return waited;
        }
        usleep(50 * 1000);
        waited += 50;
    }
    return -1;
}

/**
 * @brief Wait until an interface reports the given substring in `ip addr`.
 * @param[in] ifn interface name
 * @param[in] needle text to look for (e.g. the configured address)
 * @param[in] max_ms overall budget in milliseconds
 * @return elapsed ms on success, -1 on timeout
 */
static INT_T __wait_iface_has(CONST CHAR_T *ifn, CONST CHAR_T *needle, INT_T max_ms)
{
    CHAR_T cmd[128] = {0};
    CHAR_T out[512] = {0};
    INT_T waited = 0;

    snprintf(cmd, sizeof(cmd), "ip addr show %s 2>/dev/null", ifn);
    while (waited <= max_ms) {
        out[0] = '\0';
        if (tkl_shell(cmd, out, sizeof(out)) > 0 && strstr(out, needle) != NULL) {
            return waited;
        }
        usleep(50 * 1000);
        waited += 50;
    }
    return -1;
}

/**
 * @brief Delete the SoftAP subnet route we actually configured.
 * @note Both the stop-AP and the post-GOT_IP paths need this. It used to be a
 *       hardcoded `ip route del 192.168.176.0/24` duplicated in both places, so
 *       a cfg-supplied AP address left a stale route behind after provisioning.
 */
static VOID_T __ap_subnet_route_del(VOID_T)
{
    CHAR_T cmd[128] = {0};
    INT_T s1 = 0, s2 = 0, s3 = 0, s4 = 0;

    if (!s_ap_ip_valid || sscanf(s_ap_ip.ip, "%d.%d.%d.%d", &s1, &s2, &s3, &s4) != 4) {
        sscanf(TKL_DEF_AP_IP, "%d.%d.%d.%d", &s1, &s2, &s3, &s4);
    }
    snprintf(cmd, sizeof(cmd), "ip route del %d.%d.%d.0/24 2>/dev/null", s1, s2, s3);
    tkl_shell(cmd, NULL, 0);
}

/**
 * @brief Stop connman/NetworkManager so TKL owns wlan0 for SoftAP/STA
 * @return none
 */
static VOID_T __wifi_takeover_iface(VOID_T)
{
    /* Avoid connman fighting hostapd/wpa over the same iface. */
    tkl_shell("killall -q connmand 2>/dev/null", NULL, 0);
    tkl_shell("killall -q NetworkManager 2>/dev/null", NULL, 0);
}

/**
 * @brief Write empty wpa conf (no saved APs like open Tuya-Guest)
 * @return none
 */
static VOID_T __wpa_write_empty_conf(VOID_T)
{
    FILE *f = fopen(TKL_WPA_CONF, "w");
    if (!f) {
        return;
    }
    fprintf(f,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=1\n"
            "country=CN\n");
    fclose(f);
}

/**
 * @brief Ensure wpa_supplicant is running on STA iface
 * @return none
 */
static VOID_T tkl_wpa_ensure(VOID_T)
{
    char out[64];
    tkl_wifi_rfkill_unblock();
    if (tkl_wpa("ping", out, sizeof(out)) <= 0) {
        __wpa_write_empty_conf();
        tkl_shell("wpa_supplicant -B -i " TKL_WLAN_IFNAME
                  " -c " TKL_WPA_CONF " -D nl80211,wext >/tmp/tkl_wpa.log 2>&1",
                  NULL, 0);
        usleep(500 * 1000);
    }
}

/**
 * @brief Force restart wpa_supplicant after SoftAP teardown
 * @return none
 * @note Use empty conf — /etc/wpa_supplicant.conf may contain open guest
 *       SSIDs (e.g. Tuya-Guest) that steal association after SoftAP.
 */
static VOID_T tkl_wpa_restart(VOID_T)
{
    INT_T w;

    /*
     * Called from __sta_force_idle(), i.e. on the SoftAP start path, which the
     * SDK runs to completion before it touches BLE. The three fixed sleeps here
     * used to add a flat 1s in front of advertising; wait on the actual
     * conditions instead.
     */
    tkl_shell("killall -q wpa_supplicant 2>/dev/null", NULL, 0);
    w = 0;
    while ((w < 1000) && __proc_running("wpa_supplicant")) {
        usleep(50 * 1000);
        w += 50;
    }
    tkl_shell("rm -f /var/run/wpa_supplicant/" TKL_WLAN_IFNAME " 2>/dev/null", NULL, 0);
    tkl_shell("iw dev " TKL_WLAN_IFNAME " set type managed 2>/dev/null || "
              "iwconfig " TKL_WLAN_IFNAME " mode managed 2>/dev/null", NULL, 0);
    tkl_shell("ifconfig " TKL_WLAN_IFNAME " up 2>/dev/null", NULL, 0);
    __wpa_write_empty_conf();
    tkl_shell("wpa_supplicant -B -i " TKL_WLAN_IFNAME
              " -c " TKL_WPA_CONF " -D nl80211,wext >/tmp/tkl_wpa.log 2>&1",
              NULL, 0);
    /* Ready means the control socket answers, not merely that time passed. */
    w = 0;
    while (w < 2000) {
        CHAR_T out[64] = {0};
        if (tkl_wpa("ping", out, sizeof(out)) > 0 && strstr(out, "PONG") != NULL) {
            break;
        }
        usleep(50 * 1000);
        w += 50;
    }
    TKL_LOG("wpa restarted, ctrl ready in %dms%s", w, (w >= 2000) ? " (TIMEOUT)" : "");
}

/**
 * @brief Tear SoftAP leftovers; do not start wpa yet (caller starts with STA conf)
 * @return none
 * @note Do NOT iw-del wlan1 (hangs RTL8733BU). Do NOT use scan freq (empty+slow).
 */
static VOID_T __sta_recover_after_ap(VOID_T)
{
    tkl_shell("killall -q hostapd dnsmasq udhcpc wpa_supplicant 2>/dev/null", NULL, 0);
    usleep(300 * 1000);
    tkl_shell("rm -f /var/run/wpa_supplicant/" TKL_WLAN_IFNAME " 2>/dev/null", NULL, 0);
    tkl_shell("ifconfig " TKL_WLAN_AP_IFNAME " down 2>/dev/null", NULL, 0);
    __wifi_flush_iface(TKL_WLAN_IFNAME);
    __wifi_flush_iface(TKL_WLAN_AP_IFNAME);
    tkl_shell("iw reg set CN 2>/dev/null", NULL, 0);
    tkl_shell("iw dev " TKL_WLAN_IFNAME " set type managed 2>/dev/null || "
              "iwconfig " TKL_WLAN_IFNAME " mode managed 2>/dev/null", NULL, 0);
    tkl_shell("ifconfig " TKL_WLAN_IFNAME " up 2>/dev/null", NULL, 0);
    /* Let concurrent radio leave SoftAP channel before STA starts. */
    usleep(2000 * 1000);
    TKL_LOG("recover STA: SoftAP torn down, radio settled");
}

/**
 * @brief SoftAP→STA: write wpa conf and start wpa once (board wifi-connect.sh style)
 * @param[in] ssid target SSID
 * @param[in] passwd PSK or NULL/empty for open
 * @return OPRT_OK on success
 * @note Avoids dozens of wpa_cli calls that each time out ~2s after SoftAP.
 */
static OPERATE_RET __sta_connect_via_conf(CONST CHAR_T *ssid, CONST CHAR_T *passwd)
{
    FILE *f = NULL;
    CHAR_T out[64] = {0};
    CHAR_T hexssid[2 * WIFI_SSID_LEN + 1] = {0};

    if (!ssid || ssid[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    tkl_hex_quote(hexssid, sizeof(hexssid), (const UCHAR_T *)ssid, (int)strlen(ssid));

    f = fopen(TKL_WPA_CONF, "w");
    if (!f) {
        TKL_LOG("open %s failed", TKL_WPA_CONF);
        return OPRT_COM_ERROR;
    }
    fprintf(f,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=1\n"
            "country=CN\n"
            "ap_scan=1\n"
            "network={\n"
            "\tssid=%s\n",
            hexssid);
    if (passwd && passwd[0] != '\0') {
        /* Passphrase in quotes; hex ssid avoids quote/escape issues in SSID. */
        fprintf(f,
                "\tpsk=\"%s\"\n"
                "\tkey_mgmt=WPA-PSK\n"
                "\tscan_ssid=1\n"
                "}\n",
                passwd);
    } else {
        fprintf(f,
                "\tkey_mgmt=NONE\n"
                "\tscan_ssid=1\n"
                "}\n");
    }
    fclose(f);

    TKL_LOG("STA conf written ssid=%s, start wpa", ssid);
    tkl_shell("wpa_supplicant -B -i " TKL_WLAN_IFNAME
              " -c " TKL_WPA_CONF " -D nl80211,wext >/tmp/tkl_wpa.log 2>&1",
              NULL, 0);
    usleep(800 * 1000);

    if (tkl_wpa("ping", out, sizeof(out)) <= 0) {
        TKL_LOG("wpa ping fail after conf start, retry");
        tkl_shell("killall -q wpa_supplicant 2>/dev/null", NULL, 0);
        usleep(300 * 1000);
        tkl_shell("wpa_supplicant -B -i " TKL_WLAN_IFNAME
                  " -c " TKL_WPA_CONF " -D nl80211,wext >/tmp/tkl_wpa.log 2>&1",
                  NULL, 0);
        usleep(800 * 1000);
        if (tkl_wpa("ping", out, sizeof(out)) <= 0) {
            TKL_LOG("wpa still dead, see /tmp/tkl_wpa.log");
            return OPRT_COM_ERROR;
        }
    }

    /* Kick association; network already enabled in conf. */
    tkl_wpa("reassociate", NULL, 0);
    return OPRT_OK;
}

/**
 * @brief Drop any STA association/IP so SoftAP cannot race with old APs
 * @return none
 * @note Restart wpa once here (empty conf) — NEVER during SoftAP→STA handoff.
 */
static VOID_T __sta_force_idle(VOID_T)
{
    tkl_shell("killall -q udhcpc 2>/dev/null", NULL, 0);
    tkl_wpa_restart();
    tkl_wpa("disconnect", NULL, 0);
    tkl_wpa("remove_network all", NULL, 0);
    tkl_wpa("save_config", NULL, 0);
    __wifi_flush_iface(TKL_WLAN_IFNAME);
    s_last_stat = WSS_IDLE;
    s_conn_ssid[0] = '\0';
    TKL_LOG("STA forced idle on %s (no saved networks)", TKL_WLAN_IFNAME);
}

/**
 * @brief When App asked for a specific SSID, reject known wrong APs only
 * @param[in] st wpa_cli status text
 * @return FALSE only when status reports a different non-empty SSID
 * @note Empty ssid while COMPLETED is common briefly — do not block DHCP.
 */
static BOOL_T __ssid_is_expected(CONST CHAR_T *st)
{
    char cur[WIFI_SSID_LEN + 1] = {0};

    if (s_conn_ssid[0] == '\0') {
        return TRUE;
    }
    if (!tkl_kv(st, "ssid", cur, sizeof(cur)) || cur[0] == '\0') {
        /* Empty ssid while COMPLETED is common briefly — do not block DHCP. */
        return TRUE;
    }
    if (strcmp(cur, s_conn_ssid) != 0) {
        TKL_LOG("ignore ssid=%s (expect %s)", cur, s_conn_ssid);
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Start or restart STA DHCP client (udhcpc)
 * @param[in] force TRUE to kill and relaunch even if udhcpc already runs
 * @return none
 * @note Do NOT use udhcpc -n here: starting before association completes would
 *       make udhcpc exit once, and then never get a router IP (linkage not ready).
 */
static VOID_T __sta_dhcp_ensure(BOOL_T force)
{
    char out[64] = {0};
    time_t now = time(NULL);

    if (!force && (now - s_dhcp_last_try) < 3) {
        return;
    }
    s_dhcp_last_try = now;

    if (!force) {
        if (tkl_shell("pidof udhcpc 2>/dev/null", out, sizeof(out)) > 0 &&
            out[0] != '\0') {
            return;
        }
    }

    tkl_shell("killall -q udhcpc 2>/dev/null", NULL, 0);
    /* -b: background; -t/-T: retries; no -n so it keeps trying after SoftAP handoff. */
    tkl_shell("udhcpc -i " TKL_WLAN_IFNAME
              " -b -t 20 -T 2 -A 2 -s /usr/share/udhcpc/default.script "
              ">/tmp/tkl_udhcpc.log 2>&1", NULL, 0);
    memset(out, 0, sizeof(out));
    if (tkl_shell("pidof udhcpc 2>/dev/null", out, sizeof(out)) > 0 && out[0] != '\0') {
        TKL_LOG("udhcpc started pid=%s", out);
    } else {
        TKL_LOG("udhcpc start failed, see /tmp/tkl_udhcpc.log");
    }
}

/**
 * @brief After STA GOT_IP: clean SoftAP leftovers (must stay fast)
 * @return none
 * @note Do NOT block on ntpdate (was ~20s+ linkage not ready). But iot-dns
 *       leaf certs are NotBefore ~2026-05-15; soft_rtc often starts at 2025-01-01
 *       so set a safe wall-clock instantly, then refine via background NTP.
 *       Also take eth0 down — link[1] UP causes activate to flip linkage and
 *       fail TCP/TLS on the wrong NIC.
 */
static VOID_T __sta_post_got_ip(VOID_T)
{
    NW_IP_S tip = {0};

    tkl_shell("killall -q hostapd dnsmasq 2>/dev/null", NULL, 0);
    tkl_shell("ip route del default dev " TKL_WLAN_AP_IFNAME " 2>/dev/null", NULL, 0);
    __ap_subnet_route_del();
    __wifi_flush_iface(TKL_WLAN_AP_IFNAME);
    tkl_shell("ifconfig " TKL_WLAN_AP_IFNAME " down 2>/dev/null", NULL, 0);

    /* eth0 may have carrier without WAN; stop linkage thrash during activate. */
    tkl_shell("ip route del default dev eth0 2>/dev/null", NULL, 0);
    tkl_shell("ifconfig eth0 down 2>/dev/null", NULL, 0);

    /*
     * Instant clock fix (no network). 1778803200 = 2026-05-15 UTC (cert
     * NotBefore). 1784721600 = 2026-07-22 12:00 UTC as a safe provisional time.
     */
    tkl_shell("TS=$(date +%s 2>/dev/null); "
              "if [ -z \"$TS\" ] || [ \"$TS\" -lt 1778803200 ]; then "
              "  date -s '@1784721600' >/tmp/tkl_date.log 2>&1; "
              "fi", NULL, 0);

    /* Prefer STA default via wlan0 gateway when present. */
    tkl_shell("GW=$(ip -4 route show default dev " TKL_WLAN_IFNAME
              " 2>/dev/null | awk '/via/{print $3; exit}'); "
              "if [ -z \"$GW\" ]; then "
              "  GW=$(ip -4 route show dev " TKL_WLAN_IFNAME
              " 2>/dev/null | awk '/via/{print $3; exit}'); "
              "fi; "
              "if [ -n \"$GW\" ]; then "
              "  ip route replace default via \"$GW\" dev " TKL_WLAN_IFNAME
              " metric 10 2>/dev/null; "
              "fi", NULL, 0);

    /* Avoid large ClientHello fragments on some home APs / USB Wi-Fi. */
    tkl_shell("ip link set " TKL_WLAN_IFNAME " mtu 1400 2>/dev/null", NULL, 0);

    /* Background NTP — refine time after activate starts. */
    tkl_shell("(ntpdate -u -b -t 2 ntp.aliyun.com cn.pool.ntp.org || "
              "ntpdate -u -b -t 2 pool.ntp.org) >/tmp/tkl_ntp.log 2>&1 &", NULL, 0);

    if (tkl_wifi_get_ip(WF_STATION, &tip) == OPRT_OK) {
        TKL_LOG("post GOT_IP ip=%s mask=%s gw=%s", tip.ip, tip.mask, tip.gw);
    } else {
        TKL_LOG("post GOT_IP: get_ip failed");
    }

#if TKL_WIFI_DEBUG
    /*
     * Connectivity probe. Off by default: it fires ping x2 plus an openssl
     * handshake at the exact moment the SDK starts activating against the
     * cloud, competing for CPU and the freshly-acquired link on a device that
     * has little of either. Rebuild with -DTKL_WIFI_DEBUG=1 when diagnosing
     * "GOT_IP but activation fails".
     */
    tkl_shell("(date; ip route; cat /etc/resolv.conf; "
              "ping -c 1 -W 2 223.5.5.5; "
              "ping -c 1 -W 2 47.116.185.69; "
              "echo | timeout 5 openssl s_client -connect 47.116.185.69:443 "
              "-servername h6-ay.iot-dns.com -tls1_2 2>&1 | "
              "head -n 20) >/tmp/tkl_route.log 2>&1 &", NULL, 0);
#endif
}

/**
 * @brief Resolve SoftAP IP from SDK cfg with SDK-default fallback
 * @param[in] cfg SoftAP config from SDK
 * @param[out] out resolved IP/mask/gw
 * @return OPRT_OK on success
 */
static OPERATE_RET __resolve_ap_ip(CONST WF_AP_CFG_IF_S *cfg, NW_IP_S *out)
{
    if (!cfg || !out) {
        return OPRT_INVALID_PARM;
    }
    memset(out, 0, sizeof(NW_IP_S));
    if (cfg->ip.ip[0] != '\0') {
        strncpy(out->ip, cfg->ip.ip, sizeof(out->ip) - 1);
    } else {
        strncpy(out->ip, TKL_DEF_AP_IP, sizeof(out->ip) - 1);
    }
    if (cfg->ip.mask[0] != '\0') {
        strncpy(out->mask, cfg->ip.mask, sizeof(out->mask) - 1);
    } else {
        strncpy(out->mask, TKL_DEF_AP_MASK, sizeof(out->mask) - 1);
    }
    if (cfg->ip.gw[0] != '\0') {
        strncpy(out->gw, cfg->ip.gw, sizeof(out->gw) - 1);
    } else {
        strncpy(out->gw, out->ip, sizeof(out->gw) - 1);
    }
    return OPRT_OK;
}

/**
 * @brief Check whether ip equals saved SoftAP address
 * @param[in] ip IPv4 string
 * @return TRUE when it is the SoftAP IP
 */
static BOOL_T __is_ap_ip(CONST CHAR_T *ip)
{
    if (!ip || ip[0] == '\0' || !s_ap_ip_valid) {
        return FALSE;
    }
    return (strncmp(s_ap_ip.ip, ip, sizeof(s_ap_ip.ip)) == 0) ? TRUE : FALSE;
}

/**
 * @brief Flush IPv4 addresses on a given iface
 * @param[in] ifn interface name
 * @return none
 */
static VOID_T __wifi_flush_iface(CONST CHAR_T *ifn)
{
    CHAR_T cmd[160] = {0};
    if (!ifn || ifn[0] == '\0') {
        return;
    }
    snprintf(cmd, sizeof(cmd),
             "ip addr flush dev %s 2>/dev/null || ifconfig %s 0.0.0.0 2>/dev/null",
             ifn, ifn);
    tkl_shell(cmd, NULL, 0);
}

/**
 * @brief Ensure SoftAP iface wlan1 exists (board concurrent AP vif)
 * @return none
 * @note Matches ATK wifi_apmode.sh which runs hostapd on wlan1.
 */
static VOID_T __ensure_ap_iface(VOID_T)
{
    CHAR_T out[256] = {0};

    if (tkl_shell("ip link show " TKL_WLAN_AP_IFNAME " 2>/dev/null", out, sizeof(out)) > 0 &&
        strstr(out, TKL_WLAN_AP_IFNAME) != NULL) {
        return;
    }

    TKL_LOG("create %s as AP vif", TKL_WLAN_AP_IFNAME);
    /* Prefer adding vif on same phy as wlan0. */
    tkl_shell("iw dev " TKL_WLAN_IFNAME " interface add " TKL_WLAN_AP_IFNAME
              " type __ap 2>/tmp/tkl_iw_add.log || "
              "iw phy $(ls /sys/class/ieee80211 2>/dev/null | head -1) "
              "interface add " TKL_WLAN_AP_IFNAME " type __ap 2>>/tmp/tkl_iw_add.log",
              NULL, 0);
    usleep(300 * 1000);
}

/**
 * @brief Check hostapd is alive after SoftAP start
 * @return TRUE when hostapd process exists
 */
static BOOL_T __hostapd_running(VOID_T)
{
    if (__proc_running("hostapd")) {
        return TRUE;
    }
    return (access(TKL_HOSTAPD_PID, F_OK) == 0) ? TRUE : FALSE;
}

/**
 * @brief Build station status from wpa_cli and iface IP (filter SoftAP IP)
 * @param[in] st wpa_cli status text (may be empty)
 * @return station work status
 */
static WF_STATION_STAT_E tkl_stat_from_status(const char *st)
{
    NW_IP_S ip = {0};
    char v[32] = {0};

    if (s_wifi_mode == WWM_SOFTAP) {
        return WSS_IDLE;
    }

    if (tkl_kv(st, "wpa_state", v, sizeof(v))) {
        if (!strcmp(v, "ASSOCIATING") || !strcmp(v, "SCANNING") ||
            !strcmp(v, "AUTHENTICATING") || !strcmp(v, "ASSOCIATED") ||
            !strcmp(v, "4WAY_HANDSHAKE") || !strcmp(v, "GROUP_HANDSHAKE")) {
            return WSS_CONNECTING;
        }
        if (!strcmp(v, "COMPLETED")) {
            char wap_ip[20] = {0}; /* "255.255.255.255" + slack */

            /* Wrong AP (e.g. open Tuya-Guest) must never look like netcfg success. */
            if (!__ssid_is_expected(st)) {
                return WSS_CONNECTING;
            }
            /* Prefer real iface IP; never treat SoftAP IP as station GOT_IP. */
            if (tkl_wifi_get_ip(WF_STATION, &ip) == OPRT_OK &&
                ip.ip[0] != '\0' && !__is_ap_ip(ip.ip)) {
                return WSS_GOT_IP;
            }
            if (tkl_kv(st, "ip_address", wap_ip, sizeof(wap_ip)) &&
                wap_ip[0] != '\0' && !__is_ap_ip(wap_ip)) {
                return WSS_GOT_IP;
            }
            return WSS_CONN_SUCCESS;
        }
        if (!strcmp(v, "INTERFACE_DISABLED") || !strcmp(v, "DISCONNECTED")) {
            return WSS_IDLE;
        }
    }

    /* Fallback only when target SSID is known and matched (or no target yet). */
    if (s_conn_ssid[0] == '\0' &&
        tkl_wifi_get_ip(WF_STATION, &ip) == OPRT_OK &&
        ip.ip[0] != '\0' && !__is_ap_ip(ip.ip)) {
        return WSS_GOT_IP;
    }
    return WSS_IDLE;
}

/**
 * @brief Monitor thread: translate link/IP changes to WIFI_EVENT_CB
 * @param[in] arg unused
 * @return NULL
 */
static void *tkl_wifi_monitor(void *arg)
{
    (void)arg;
    char st[512];
    time_t last_progress_log = 0;

    while (s_mon_run) {
        if (s_wifi_mode == WWM_STATION || s_wifi_mode == WWM_STATIONAP) {
            WF_STATION_STAT_E cur = WSS_IDLE;
            BOOL_T connecting = (s_conn_ssid[0] != '\0') && (s_conn_start_ts > 0);
            BOOL_T ssid_ok = FALSE;
            BOOL_T completed = FALSE;
            char ws[32] = {0};
            time_t now;

            /*
             * Clear every pass: the blocks below read st[] unconditionally, so
             * a failed wpa_cli call used to leave them parsing the previous
             * iteration's text — or uninitialised stack on the very first one.
             */
            st[0] = '\0';
            (void)tkl_wpa("status", st, sizeof(st));
            cur = tkl_stat_from_status(st[0] ? st : NULL);

            /* Parse once per pass instead of re-scanning the text 4-5 times. */
            (void)tkl_kv(st, "wpa_state", ws, sizeof(ws));
            ssid_ok = __ssid_is_expected(st);
            completed = (strcmp(ws, "COMPLETED") == 0) ? TRUE : FALSE;
            now = time(NULL);

            /*
             * Kick DHCP once L2 is up (or still connecting to target).
             * Only skip when status clearly shows a wrong SSID.
             */
            if (s_conn_ssid[0] != '\0' && ssid_ok &&
                (cur == WSS_CONN_SUCCESS || cur == WSS_CONNECTING || completed)) {
                BOOL_T force_dhcp = FALSE;
                if ((cur == WSS_CONN_SUCCESS || completed) && s_conn_start_ts > 0 &&
                    (now - s_dhcp_last_try) >= 3) {
                    force_dhcp = TRUE;
                }
                __sta_dhcp_ensure(force_dhcp);
            }

            /*
             * Recovery, NOT debug: the SoftAP->STA handoff often parks wpa in
             * INACTIVE/DISCONNECTED with nothing driving it forward. This used
             * to live inside the field-debug block, so silencing the logs also
             * silently removed the only thing that unstuck provisioning.
             */
            if (connecting && cur != WSS_GOT_IP &&
                (!strcmp(ws, "INACTIVE") || !strcmp(ws, "DISCONNECTED"))) {
                if ((now - s_reassoc_last_try) >= TKL_REASSOC_MIN_SEC) {
                    s_reassoc_last_try = now;
                    TKL_LOG("wpa stuck in %s -> scan+reassociate ssid=%s (%lds since connect)",
                            ws, s_conn_ssid, (long)(now - s_conn_start_ts));
                    tkl_wpa("scan", NULL, 0);
                    tkl_wpa("reassociate", NULL, 0);
                }
            }

            /* Progress trace while waiting for GOT_IP (throttled, not modulo-timed). */
            if (connecting && cur != WSS_GOT_IP &&
                (now - last_progress_log) >= TKL_PROGRESS_LOG_SEC) {
                char sid[WIFI_SSID_LEN + 1] = {0};
                char fq[16] = {0};
                char dhcp[32] = {0};
                NW_IP_S tip = {0};

                last_progress_log = now;
                (void)tkl_kv(st, "ssid", sid, sizeof(sid));
                (void)tkl_kv(st, "freq", fq, sizeof(fq));
                (void)tkl_wifi_get_ip(WF_STATION, &tip);
                tkl_shell("pidof udhcpc 2>/dev/null", dhcp, sizeof(dhcp));
                dhcp[strcspn(dhcp, "\r\n")] = '\0';
                TKL_LOG("wait IP [%lds] stat=%s wpa=%s ssid=%s freq=%s ip=%s udhcpc=%s",
                        (long)(now - s_conn_start_ts), __stat_name(cur),
                        ws[0] ? ws : "-", sid[0] ? sid : "-", fq[0] ? fq : "-",
                        tip.ip[0] ? tip.ip : "-", dhcp[0] ? dhcp : "-");
            }

            if (cur != s_last_stat) {
                TKL_LOG("station state %s -> %s%s", __stat_name(s_last_stat), __stat_name(cur),
                        s_conn_ssid[0] ? "" : " (no target ssid)");
            }

            if (cur != s_last_stat && s_wifi_cb) {
                if (cur == WSS_GOT_IP) {
                    NW_IP_S tip = {0};
                    if (tkl_wifi_get_ip(WF_STATION, &tip) == OPRT_OK) {
                        TKL_LOG("station GOT_IP %s after %lds -> WFE_CONNECTED", tip.ip,
                                s_conn_start_ts > 0 ? (long)(now - s_conn_start_ts) : 0L);
                    }
                    __sta_post_got_ip();
                    s_wifi_cb(WFE_CONNECTED, NULL);
                    s_conn_fail_reported = FALSE;
                    s_conn_start_ts = 0;
                } else if (cur == WSS_IDLE && s_last_stat >= WSS_CONNECTING) {
                    TKL_LOG("station link lost -> WFE_DISCONNECTED");
                    s_wifi_cb(WFE_DISCONNECTED, NULL);
                }
            }

            /* Report connect failure once after timeout. */
            if (s_conn_start_ts > 0 && !s_conn_fail_reported && cur != WSS_GOT_IP &&
                (cur == WSS_CONNECTING || cur == WSS_CONN_SUCCESS || cur == WSS_IDLE)) {
                if ((now - s_conn_start_ts) >= TKL_CONNECT_FAIL_SEC) {
                    TKL_LOG("station connect timeout after %ds (stat=%s wpa=%s) -> WFE_CONNECT_FAILED",
                            TKL_CONNECT_FAIL_SEC, __stat_name(cur), ws[0] ? ws : "-");
                    if (s_wifi_cb) {
                        s_wifi_cb(WFE_CONNECT_FAILED, NULL);
                    }
                    s_conn_fail_reported = TRUE;
                }
            }
            s_last_stat = cur;
        }
        sleep(1);
    }
    return NULL;
}

/**
 * @brief Encode binary as lowercase hex string (wpa ssid=hex form)
 * @param[out] dst output buffer
 * @param[in] dstsz output size
 * @param[in] src bytes
 * @param[in] srclen byte count
 * @return none
 */
static VOID_T tkl_hex_quote(char *dst, int dstsz, const UCHAR_T *src, int srclen)
{
    int i, n = 0;
    for (i = 0; i < srclen && n + 2 < dstsz; i++) {
        n += snprintf(dst + n, (size_t)dstsz - (size_t)n, "%02x", src[i]);
    }
    dst[n] = 0;
}

/* ---------------------------------------------------------------------------
 * Public TKL API
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize Wi-Fi TKL and start link monitor
 * @param[in] cb station event callback
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_init(WIFI_EVENT_CB cb)
{
    s_wifi_cb = cb;
    s_wifi_mode = WWM_STATION;
    s_last_stat = WSS_IDLE;
    memset(&s_ap_ip, 0, sizeof(s_ap_ip));
    s_ap_ip_valid = FALSE;
    tkl_wifi_rfkill_unblock();
    __wifi_takeover_iface();
    tkl_shell("ifconfig " TKL_WLAN_IFNAME " up 2>/dev/null", NULL, 0);
    tkl_wpa_ensure();
    if (!s_mon_run) {
        s_mon_run = TRUE;
        pthread_create(&s_mon_tid, NULL, tkl_wifi_monitor, NULL);
        pthread_detach(s_mon_tid);
    }
    return OPRT_OK;
}

/**
 * @brief Scan nearby APs via wpa_cli
 * @param[in] ssid optional SSID filter (NULL = all)
 * @param[out] ap_ary allocated AP array
 * @param[out] num AP count
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_scan_ap(CONST SCHAR_T *ssid, AP_IF_S **ap_ary, UINT_T *num)
{
    char out[4096];
    if (!ap_ary || !num) {
        return OPRT_INVALID_PARM;
    }
    tkl_wpa("scan", NULL, 0);
    usleep(400 * 1000);
    int n = tkl_wpa("scan_results", out, sizeof(out));
    if (n <= 0) {
        *ap_ary = NULL;
        *num = 0;
        return OPRT_OK;
    }

    int lines = 0;
    for (char *p = out; *p; p++) {
        if (*p == '\n') {
            lines++;
        }
    }
    if (lines <= 1) {
        *ap_ary = NULL;
        *num = 0;
        return OPRT_OK;
    }

    AP_IF_S *arr = (AP_IF_S *)tkl_system_malloc(sizeof(AP_IF_S) * (UINT_T)lines);
    if (!arr) {
        return OPRT_RESOURCE_NOT_READY;
    }
    memset(arr, 0, sizeof(AP_IF_S) * (UINT_T)lines);
    UINT_T cnt = 0;
    char *line = strtok(out, "\n");
    if (line) {
        line = strtok(NULL, "\n");
    }
    for (; line && cnt < (UINT_T)lines; line = strtok(NULL, "\n")) {
        AP_IF_S *ap = &arr[cnt];
        char bssid[32] = {0}, flags[64] = {0};
        int sig = 0, freq = 0;
        if (sscanf(line, "%31s %d %d %63s", bssid, &freq, &sig, flags) < 4) {
            continue;
        }
        const char *sp = strstr(line, flags);
        if (sp) {
            sp += strlen(flags);
            while (*sp == ' ') {
                sp++;
            }
        } else {
            sp = "";
        }
        int slen = (int)strlen(sp);
        if (slen > WIFI_SSID_LEN) {
            slen = WIFI_SSID_LEN;
        }
        memcpy(ap->ssid, sp, (size_t)slen);
        ap->ssid[slen] = 0;
        ap->s_len = (UCHAR_T)slen;
        ap->rssi = (SCHAR_T)sig;
        sscanf(bssid, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
               &ap->bssid[0], &ap->bssid[1], &ap->bssid[2],
               &ap->bssid[3], &ap->bssid[4], &ap->bssid[5]);
        ap->channel = (freq >= 2412) ? (UCHAR_T)((freq - 2412) / 5 + 1) : 0;
        ap->security = strstr(flags, "WPA2") ? WAAM_WPA2_PSK :
                       strstr(flags, "WPA")  ? WAAM_WPA_PSK  :
                       strstr(flags, "WEP")  ? WAAM_WEP      : WAAM_OPEN;
        /*
         * Full-string compare. strncmp() limited to the *scanned* SSID length
         * made any AP whose name is a prefix of the target match it — scanning
         * for "MyNetwork" would accept a nearby "MyNet".
         */
        if (ssid == NULL || strcmp((const char *)ap->ssid, (const char *)ssid) == 0) {
            cnt++;
        }
    }
    *ap_ary = arr;
    *num = cnt;
    TKL_LOG("scan done: %u/%d ap%s%s", cnt, lines - 1, (cnt == 1) ? "" : "s",
            ssid ? " (filtered)" : "");
    return OPRT_OK;
}

/**
 * @brief Scan AP on a given channel (channel ignored on this platform)
 * @param[in] ssid optional SSID filter
 * @param[in] channel unused
 * @param[out] ap_ary allocated AP array
 * @param[out] num AP count
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_scan_ap_channel(CONST SCHAR_T *ssid, CONST UCHAR_T channel, AP_IF_S **ap_ary, UINT_T *num)
{
    (void)channel;
    return tkl_wifi_scan_ap(ssid, ap_ary, num);
}

/**
 * @brief Release buffer allocated by scan AP APIs
 * @param[in] ap buffer from tkl_wifi_scan_ap
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_release_ap(AP_IF_S *ap)
{
    if (ap) {
        tkl_system_free(ap);
    }
    return OPRT_OK;
}

/**
 * @brief Start SoftAP for hotspot provisioning
 * @param[in] cfg SoftAP config from SDK (must honor cfg->ip)
 * @return OPRT_OK on success
 * @note App selects encryption by SoftAP IP segment; default is 192.168.176.1.
 */
OPERATE_RET tkl_wifi_start_ap(CONST WF_AP_CFG_IF_S *cfg)
{
    CHAR_T cmd[256] = {0};
    CHAR_T ip_prefix[16] = {0};
    INT_T seg1 = 0, seg2 = 0, seg3 = 0, seg4 = 0;
    UCHAR_T chan = TKL_DEF_AP_CHAN;
    UCHAR_T max_conn = TKL_DEF_AP_MAX_CONN;
    USHORT_T beacon = TKL_DEF_AP_BEACON;
    INT_T wpa = 0;
    FILE *f = NULL;
    FILE *d = NULL;
    NW_IP_S ap_ip;

    if (!cfg || cfg->ssid[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    __resolve_ap_ip(cfg, &ap_ip);
    memcpy(&s_ap_ip, &ap_ip, sizeof(s_ap_ip));
    s_ap_ip_valid = TRUE;

    chan = cfg->chan ? cfg->chan : TKL_DEF_AP_CHAN;
    max_conn = cfg->max_conn ? cfg->max_conn : TKL_DEF_AP_MAX_CONN;
    beacon = cfg->ms_interval ? cfg->ms_interval : TKL_DEF_AP_BEACON;
    if (cfg->p_len > 0 && cfg->md != WAAM_OPEN) {
        wpa = (cfg->md == WAAM_WPA_PSK) ? 1 : 2;
    }

    sscanf(ap_ip.ip, "%d.%d.%d.%d", &seg1, &seg2, &seg3, &seg4);
    snprintf(ip_prefix, sizeof(ip_prefix), "%d.%d.%d", seg1, seg2, seg3);

    TKL_LOG("start ap ssid=%s if=%s chan=%u ip=%s mask=%s gw=%s wpa=%d",
            (char *)cfg->ssid, TKL_WLAN_AP_IFNAME, chan,
            ap_ip.ip, ap_ip.mask, ap_ip.gw, wpa);

    tkl_wifi_rfkill_unblock();
    __wifi_takeover_iface();
    /*
     * SoftAP on wlan1 only (board design). Keep wpa_supplicant on wlan0 —
     * killing it and reusing wlan0 for hostapd is what broke STA DHCP.
     */
    tkl_shell("killall -q connmand 2>/dev/null", NULL, 0);
    tkl_shell("killall -q hostapd dnsmasq 2>/dev/null", NULL, 0);
    /* eth0 link-up makes SDK prefer wired linkage during SoftAP. */
    tkl_shell("ifconfig eth0 down 2>/dev/null", NULL, 0);
    /*
     * SoftAP is on wlan1, but wlan0 must NOT stay associated to open APs
     * (Tuya-Guest etc.) or udhcpc will re-grab their IP and fake GOT_IP.
     */
    __sta_force_idle();
    __ensure_ap_iface();
    {
        CHAR_T chk[128] = {0};
        if (tkl_shell("ip link show " TKL_WLAN_AP_IFNAME " 2>/dev/null", chk, sizeof(chk)) <= 0 ||
            strstr(chk, TKL_WLAN_AP_IFNAME) == NULL) {
            TKL_LOG("%s missing, see /tmp/tkl_iw_add.log", TKL_WLAN_AP_IFNAME);
            s_ap_ip_valid = FALSE;
            return OPRT_COM_ERROR;
        }
    }
    __wifi_flush_iface(TKL_WLAN_AP_IFNAME);
    __wifi_flush_iface(TKL_WLAN_IFNAME);

    f = fopen(TKL_HOSTAPD_CONF, "w");
    if (!f) {
        TKL_LOG("open %s failed", TKL_HOSTAPD_CONF);
        s_ap_ip_valid = FALSE;
        return OPRT_COM_ERROR;
    }
    fprintf(f,
            "interface=%s\n"
            "driver=nl80211\n"
            "ssid=%s\n"
            "country_code=CN\n"
            "hw_mode=g\n"
            "channel=%u\n"
            "beacon_int=%u\n"
            "max_num_sta=%u\n"
            "ignore_broadcast_ssid=%u\n"
            "wmm_enabled=0\n"
            "auth_algs=1\n",
            TKL_WLAN_AP_IFNAME,
            (char *)cfg->ssid,
            (unsigned)chan,
            (unsigned)beacon,
            (unsigned)max_conn,
            cfg->ssid_hidden ? 1U : 0U);
    if (wpa > 0) {
        fprintf(f,
                "wpa=%d\n"
                "wpa_passphrase=%s\n"
                "wpa_key_mgmt=WPA-PSK\n"
                "wpa_pairwise=TKIP CCMP\n"
                "rsn_pairwise=CCMP\n",
                wpa, (char *)cfg->passwd);
    } else {
        fprintf(f, "wpa=0\nmacaddr_acl=0\n");
    }
    fclose(f);

    /* Match board wifi_apmode.sh: ifconfig wlan1 up then assign SoftAP IP. */
    tkl_shell("ifconfig " TKL_WLAN_AP_IFNAME " up 2>/dev/null", NULL, 0);
    snprintf(cmd, sizeof(cmd), "ifconfig %s %s netmask %s up",
             TKL_WLAN_AP_IFNAME, ap_ip.ip, ap_ip.mask);
    tkl_shell(cmd, NULL, 0);
    /* Wait for the address to actually land rather than guessing 500ms. */
    {
        INT_T w = __wait_iface_has(TKL_WLAN_AP_IFNAME, ap_ip.ip, 2000);
        if (w < 0) {
            TKL_LOG("WARN: %s did not report %s within 2s", TKL_WLAN_AP_IFNAME, ap_ip.ip);
        } else {
            TKL_LOG("%s addr %s ready in %dms", TKL_WLAN_AP_IFNAME, ap_ip.ip, w);
        }
    }

    snprintf(cmd, sizeof(cmd), "hostapd -B -P %s %s >/tmp/tkl_hostapd.log 2>&1",
             TKL_HOSTAPD_PID, TKL_HOSTAPD_CONF);
    tkl_shell(cmd, NULL, 0);
    {
        /*
         * Poll __hostapd_running() rather than pidof alone: it also accepts the
         * -P pidfile, which is the "hostapd really came up" check that notes
         * item 11 added after hostapd was seen reporting false success.
         */
        INT_T w = 0;
        while ((w < 3000) && !__hostapd_running()) {
            usleep(50 * 1000);
            w += 50;
        }
        if (!__hostapd_running()) {
            TKL_LOG("hostapd failed to start within 3s, see /tmp/tkl_hostapd.log");
            /* wlan1 is already up holding the AP address — roll it back. */
            tkl_wifi_stop_ap();
            return OPRT_COM_ERROR;
        }
        TKL_LOG("hostapd up on %s in %dms", TKL_WLAN_AP_IFNAME, w);
    }

    d = fopen(TKL_DNSMASQ_CONF, "w");
    if (!d) {
        TKL_LOG("open %s failed", TKL_DNSMASQ_CONF);
        tkl_wifi_stop_ap();
        return OPRT_COM_ERROR;
    }
    /*
     * DHCP only (port=0). Must set dhcp-leasefile to a writable path —
     * default /var/lib/misc/dnsmasq.leases often missing on this rootfs,
     * which makes dnsmasq exit immediately (phone associates but gets no IP).
     */
    fprintf(d,
            "interface=%s\n"
            "port=0\n"
            "dhcp-range=%s.100,%s.200,%s,2h\n"
            "dhcp-option=3,%s\n"
            "dhcp-option=6,%s\n"
            "dhcp-leasefile=/tmp/tkl_dnsmasq.leases\n"
            "pid-file=/tmp/tkl_dnsmasq.pid\n",
            TKL_WLAN_AP_IFNAME,
            ip_prefix, ip_prefix, ap_ip.mask,
            ap_ip.gw, ap_ip.gw);
    fclose(d);
    unlink("/tmp/tkl_dnsmasq.leases");
    /* dnsmasq daemonizes itself; match board wifi_apmode.sh style (-C). */
    tkl_shell("dnsmasq -C " TKL_DNSMASQ_CONF " >/tmp/tkl_dnsmasq.log 2>&1", NULL, 0);
    {
        INT_T w = __wait_proc("dnsmasq", 2000);
        if (w < 0) {
            TKL_LOG("dnsmasq failed to start within 2s, see /tmp/tkl_dnsmasq.log");
            /*
             * SoftAP radio is up but the phone would never get an IP. Tear the
             * AP back down instead of returning an error with hostapd still
             * beaconing a dead SmartLife-xxxx that the user can join but not use.
             */
            tkl_wifi_stop_ap();
            return OPRT_COM_ERROR;
        }
    }

    TKL_LOG("dnsmasq up, DHCP pool %s.100-%s.200", ip_prefix, ip_prefix);

    s_wifi_mode = WWM_SOFTAP;
    s_last_stat = WSS_IDLE;
    TKL_LOG("SoftAP up: %s @ %s/%s (hostapd+dnsmasq)",
            (char *)cfg->ssid, TKL_WLAN_AP_IFNAME, ap_ip.ip);
    return OPRT_OK;
}

/**
 * @brief Stop SoftAP after receiving netcfg payload
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_stop_ap(VOID_T)
{
    TKL_LOG("stop ap on %s", TKL_WLAN_AP_IFNAME);
    tkl_shell("killall -q dnsmasq 2>/dev/null", NULL, 0);
    tkl_shell("killall -q hostapd 2>/dev/null", NULL, 0);
    unlink(TKL_HOSTAPD_PID);
    usleep(200 * 1000);
    /* Tear down wlan1 SoftAP only. Do NOT `iw del wlan1` — hangs RTL8733BU. */
    tkl_shell("ip route del default dev " TKL_WLAN_AP_IFNAME " 2>/dev/null", NULL, 0);
    __ap_subnet_route_del();
    __wifi_flush_iface(TKL_WLAN_AP_IFNAME);
    tkl_shell("ifconfig " TKL_WLAN_AP_IFNAME " down 2>/dev/null", NULL, 0);
    usleep(100 * 1000);
    s_wifi_mode = WWM_STATION;
    TKL_LOG("stop ap done");
    /* Keep s_ap_ip for GOT_IP filtering until next start_ap. */
    return OPRT_OK;
}

/**
 * @brief Set current channel (not used on this platform)
 * @param[in] chan channel
 * @return OPRT_OK
 */
OPERATE_RET tkl_wifi_set_cur_channel(CONST UCHAR_T chan)
{
    (void)chan;
    return OPRT_OK;
}

/**
 * @brief Get current channel from wpa status
 * @param[out] chan channel
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_get_cur_channel(UCHAR_T *chan)
{
    if (!chan) {
        return OPRT_INVALID_PARM;
    }
    char st[256];
    *chan = 0;
    if (tkl_wpa("status", st, sizeof(st)) > 0) {
        char v[16] = {0};
        if (tkl_kv(st, "freq", v, sizeof(v))) {
            int f = atoi(v);
            if (f >= 2412) {
                *chan = (UCHAR_T)((f - 2412) / 5 + 1);
            }
        }
    }
    return OPRT_OK;
}

/**
 * @brief Enable sniffer (not supported on this USB Wi-Fi path)
 * @param[in] en enable flag
 * @param[in] cb sniffer callback
 * @return OPRT_NOT_SUPPORTED
 */
OPERATE_RET tkl_wifi_set_sniffer(CONST BOOL_T en, CONST SNIFFER_CALLBACK cb)
{
    (void)en;
    (void)cb;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Get Wi-Fi IP info for AP or STA iface
 * @param[in] wf WF_AP or WF_STATION
 * @param[out] ip IP/mask/gw
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_get_ip(CONST WF_IF_E wf, NW_IP_S *ip)
{
    if (!ip) {
        return OPRT_INVALID_PARM;
    }
    memset(ip, 0, sizeof(NW_IP_S));
    const char *ifn = (wf == WF_AP) ? TKL_WLAN_AP_IFNAME : TKL_WLAN_IFNAME;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return OPRT_COM_ERROR;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifn, sizeof(ifr.ifr_name) - 1);
    OPERATE_RET rt = OPRT_OK;
    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        strncpy(ip->ip, inet_ntoa(sin->sin_addr), sizeof(ip->ip) - 1);
    } else {
        rt = OPRT_COM_ERROR;
    }
    if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        strncpy(ip->mask, inet_ntoa(sin->sin_addr), sizeof(ip->mask) - 1);
    }
    /* SoftAP: use configured gw. STA: read default route, not broadcast. */
    if (wf == WF_AP && s_ap_ip_valid && s_ap_ip.gw[0] != '\0') {
        strncpy(ip->gw, s_ap_ip.gw, sizeof(ip->gw) - 1);
    } else {
        CHAR_T route[128] = {0};
        if (tkl_shell("ip route show default 2>/dev/null | awk '{print $3; exit}'",
                      route, sizeof(route)) > 0 && route[0] != '\0') {
            CHAR_T *nl = strchr(route, '\n');
            if (nl) {
                *nl = '\0';
            }
            strncpy(ip->gw, route, sizeof(ip->gw) - 1);
        }
    }
    close(sock);

    /* SoftAP mode: if ioctl failed but we configured IP, return saved values. */
    if (wf == WF_AP && rt != OPRT_OK && s_ap_ip_valid) {
        memcpy(ip, &s_ap_ip, sizeof(NW_IP_S));
        return OPRT_OK;
    }
    return rt;
}

/**
 * @brief Get IPv6 (not supported)
 * @return OPRT_NOT_SUPPORTED
 */
OPERATE_RET tkl_wifi_get_ipv6(CONST WF_IF_E wf, NW_IP_TYPE type, NW_IP_S *ip)
{
    (void)wf;
    (void)type;
    if (ip) {
        memset(ip, 0, sizeof(NW_IP_S));
    }
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Set IP (not used; SoftAP uses start_ap cfg)
 * @return OPRT_NOT_SUPPORTED
 */
OPERATE_RET tkl_wifi_set_ip(CONST WF_IF_E wf, NW_IP_S *ip)
{
    (void)wf;
    (void)ip;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Set MAC (not supported)
 * @return OPRT_NOT_SUPPORTED
 */
OPERATE_RET tkl_wifi_set_mac(CONST WF_IF_E wf, CONST NW_MAC_S *mac)
{
    (void)wf;
    (void)mac;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Get MAC address of Wi-Fi iface
 * @param[in] wf WF_AP or WF_STATION
 * @param[out] mac MAC bytes
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_get_mac(CONST WF_IF_E wf, NW_MAC_S *mac)
{
    if (!mac) {
        return OPRT_INVALID_PARM;
    }
    const char *ifn = (wf == WF_AP) ? TKL_WLAN_AP_IFNAME : TKL_WLAN_IFNAME;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return OPRT_COM_ERROR;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifn, sizeof(ifr.ifr_name) - 1);
    OPERATE_RET rt = (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) ? OPRT_OK : OPRT_COM_ERROR;
    close(sock);
    if (rt == OPRT_OK) {
        memcpy(mac->mac, ifr.ifr_hwaddr.sa_data, 6);
    }
    return rt;
}

/**
 * @brief Set Wi-Fi work mode (AP / Station)
 * @param[in] mode work mode
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_set_work_mode(CONST WF_WK_MD_E mode)
{
    TKL_LOG("set work mode %d", (int)mode);
    s_wifi_mode = mode;
    if (mode == WWM_STATION || mode == WWM_STATIONAP) {
        tkl_shell("iwconfig " TKL_WLAN_IFNAME " mode managed 2>/dev/null", NULL, 0);
        tkl_wpa_ensure();
    }
    return OPRT_OK;
}

/**
 * @brief Get Wi-Fi work mode
 * @param[out] mode work mode
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_get_work_mode(WF_WK_MD_E *mode)
{
    if (!mode) {
        return OPRT_INVALID_PARM;
    }
    *mode = s_wifi_mode;
    return OPRT_OK;
}

/**
 * @brief Get fast-connect AP info (not supported)
 * @return OPRT_NOT_SUPPORTED
 */
OPERATE_RET tkl_wifi_get_connected_ap_info(FAST_WF_CONNECTED_AP_INFO_T **fast_ap_info)
{
    (void)fast_ap_info;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Get connected BSSID
 * @param[out] mac BSSID
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_get_bssid(UCHAR_T *mac)
{
    if (!mac) {
        return OPRT_INVALID_PARM;
    }
    char st[256];
    memset(mac, 0, 6);
    if (tkl_wpa("status", st, sizeof(st)) > 0) {
        char v[32] = {0};
        if (tkl_kv(st, "bssid", v, sizeof(v))) {
            sscanf(v, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
        }
    }
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_set_country_code(CONST COUNTRY_CODE_E ccode)
{
    (void)ccode;
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_get_country_code(UCHAR_T *ccode)
{
    if (ccode) {
        strcpy((char *)ccode, "CN");
    }
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_set_country_code_v2(CONST UCHAR_T *ccode)
{
    (void)ccode;
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_set_rf_calibrated(VOID_T)
{
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_set_lp_mode(CONST BOOL_T enable, CONST UCHAR_T dtim)
{
    (void)enable;
    (void)dtim;
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_station_fast_connect(CONST FAST_WF_CONNECTED_AP_INFO_T *fast_ap_info)
{
    (void)fast_ap_info;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Connect router after SoftAP netcfg payload is received
 * @param[in] ssid router SSID
 * @param[in] passwd router password (may be empty for open)
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_station_connect(CONST SCHAR_T *ssid, CONST SCHAR_T *passwd)
{
    OPERATE_RET ret;

    if (!ssid || ssid[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    /* Record target SSID first so status polling cannot accept a wrong AP. */
    strncpy(s_conn_ssid, (const char *)ssid, sizeof(s_conn_ssid) - 1);
    s_conn_ssid[sizeof(s_conn_ssid) - 1] = 0;
    s_wifi_mode = WWM_STATION;
    s_last_stat = WSS_CONNECTING;
    s_conn_start_ts = time(NULL);
    s_conn_fail_reported = FALSE;
    s_dhcp_last_try = 0;
    s_reassoc_last_try = 0;

    TKL_LOG("station connect begin ssid=%s pw=%s", (const char *)ssid,
            (passwd && passwd[0]) ? "yes" : "(open)");

    /*
     * SoftAP→STA handoff: stop AP, settle radio, start wpa with one conf.
     * Do not use wpa_cli scan freq / many set_network calls (hang ~20s, empty BSS).
     */
    tkl_wifi_stop_ap();
    tkl_wifi_rfkill_unblock();
    __wifi_takeover_iface();
    __sta_recover_after_ap();

    ret = __sta_connect_via_conf((CONST CHAR_T *)ssid,
                                 passwd ? (CONST CHAR_T *)passwd : "");
    if (ret != OPRT_OK) {
        return ret;
    }

    __sta_dhcp_ensure(TRUE);
    TKL_LOG("station connect issued, wait monitor for GOT_IP");
    return OPRT_OK;
}

/**
 * @brief Disconnect from router
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_station_disconnect(VOID_T)
{
    BOOL_T was_up = (s_last_stat >= WSS_CONNECTING) ? TRUE : FALSE;

    TKL_LOG("station disconnect requested (was %s ssid=%s)", __stat_name(s_last_stat),
            s_conn_ssid[0] ? s_conn_ssid : "-");

    s_last_stat = WSS_IDLE;
    s_conn_start_ts = 0;
    s_conn_fail_reported = FALSE;
    /*
     * Clear the target too: the monitor keys its DHCP kicking off s_conn_ssid,
     * so leaving it set kept udhcpc being relaunched for an SSID we had just
     * deliberately dropped.
     */
    s_conn_ssid[0] = '\0';

    tkl_wpa("disconnect", NULL, 0);
    tkl_shell("killall -q udhcpc 2>/dev/null", NULL, 0);
    /*
     * Drop the address too. tkl_stat_from_status() falls back to "has a
     * non-AP IP => GOT_IP" when no target SSID is set, so a lingering lease on
     * the interface would make the very next monitor pass re-report
     * WFE_CONNECTED right after we told the SDK the link was down.
     */
    __wifi_flush_iface(TKL_WLAN_IFNAME);

    /* Only report a drop if we had actually been up — avoids a phantom
     * WFE_DISCONNECTED on a disconnect issued from the idle state. */
    if (was_up && s_wifi_cb) {
        s_wifi_cb(WFE_DISCONNECTED, NULL);
    }
    return OPRT_OK;
}

/**
 * @brief Get connected AP RSSI
 * @param[out] rssi signal
 * @return OPRT_OK on success
 */
OPERATE_RET tkl_wifi_station_get_conn_ap_rssi(SCHAR_T *rssi)
{
    if (!rssi) {
        return OPRT_INVALID_PARM;
    }
    char st[256];
    *rssi = 0;
    if (tkl_wpa("status", st, sizeof(st)) > 0) {
        char v[16] = {0};
        if (tkl_kv(st, "signal", v, sizeof(v))) {
            *rssi = (SCHAR_T)atoi(v);
        }
    }
    return OPRT_OK;
}

/**
 * @brief Get station work status; only router IP yields WSS_GOT_IP
 * @param[out] stat station status
 * @return OPRT_OK on success
 * @note SoftAP IP must not be reported as GOT_IP or activation will time out.
 */
OPERATE_RET tkl_wifi_station_get_status(WF_STATION_STAT_E *stat)
{
    if (!stat) {
        return OPRT_INVALID_PARM;
    }
    char st[256];
    if (tkl_wpa("status", st, sizeof(st)) > 0) {
        *stat = tkl_stat_from_status(st);
    } else {
        *stat = tkl_stat_from_status(NULL);
    }
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_send_mgnt(CONST UCHAR_T *buf, CONST UINT_T len)
{
    (void)buf;
    (void)len;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_register_recv_mgnt_callback(CONST BOOL_T enable, CONST WIFI_REV_MGNT_CB recv_cb)
{
    (void)enable;
    (void)recv_cb;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_ioctl(WF_IOCTL_CMD_E cmd, VOID *args)
{
    if (cmd == WFI_CONNECT_CMD && args) {
        WF_IOCTL_CONN_T *c = (WF_IOCTL_CONN_T *)args;
        return tkl_wifi_station_connect((SCHAR_T *)c->ssid, (SCHAR_T *)c->passwd);
    }
    return OPRT_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * TAL ops table
 * --------------------------------------------------------------------------- */
/**
 * @brief Adapter for DESC field whose return type is BOOL_T
 * @return TRUE when RF is considered calibrated
 */
STATIC BOOL_T __tkl_wifi_rf_calibrated_bool(VOID_T)
{
    return (tkl_wifi_set_rf_calibrated() == OPRT_OK) ? TRUE : FALSE;
}

/**
 * TAL Wi-Fi ops table (must keep the symbol name TKL_WIFI).
 */
CONST TKL_WIFI_DESC_T TKL_WIFI = {
    .init                        = tkl_wifi_init,
    .scan_ap                     = tkl_wifi_scan_ap,
    .scan_ap_channel             = tkl_wifi_scan_ap_channel,
    .release_ap                  = tkl_wifi_release_ap,
    .start_ap                    = tkl_wifi_start_ap,
    .stop_ap                     = tkl_wifi_stop_ap,
    .set_cur_channel             = tkl_wifi_set_cur_channel,
    .get_cur_channel             = tkl_wifi_get_cur_channel,
    .set_sniffer                 = tkl_wifi_set_sniffer,
    .set_ip                      = tkl_wifi_set_ip,
    .get_ip                      = tkl_wifi_get_ip,
    .get_ipv6                    = tkl_wifi_get_ipv6,
    .set_mac                     = tkl_wifi_set_mac,
    .get_mac                     = tkl_wifi_get_mac,
    .set_work_mode               = tkl_wifi_set_work_mode,
    .get_work_mode               = tkl_wifi_get_work_mode,
    .get_connected_ap_info       = tkl_wifi_get_connected_ap_info,
    .get_bssid                   = tkl_wifi_get_bssid,
    .set_country_code            = tkl_wifi_set_country_code,
    .set_country_code_v2         = tkl_wifi_set_country_code_v2,
    .get_country_code            = tkl_wifi_get_country_code,
    .set_lp_mode                 = tkl_wifi_set_lp_mode,
    .set_rf_calibrated           = __tkl_wifi_rf_calibrated_bool,
    .station_fast_connect        = tkl_wifi_station_fast_connect,
    .station_connect             = tkl_wifi_station_connect,
    .station_disconnect          = tkl_wifi_station_disconnect,
    .station_get_conn_ap_rssi    = tkl_wifi_station_get_conn_ap_rssi,
    .station_get_status          = tkl_wifi_station_get_status,
    .send_mgnt                   = tkl_wifi_send_mgnt,
    .register_recv_mgnt_callback = tkl_wifi_register_recv_mgnt_callback,
    .ioctl                       = tkl_wifi_ioctl,
};

/**
 * @brief Return Wi-Fi TKL description used by object-manage style init
 * @return pointer to TKL_WIFI ops table
 */
TKL_WIFI_DESC_T *tkl_wifi_desc_get(VOID_T)
{
    return (TKL_WIFI_DESC_T *)&TKL_WIFI;
}

/**
 * @brief Return hostap TKL description (unused; SoftAP via start_ap)
 * @return NULL
 */
TKL_WIFI_HOSTAP_DESC_T *tkl_wifi_hostap_desc_get(VOID_T)
{
    return NULL;
}
