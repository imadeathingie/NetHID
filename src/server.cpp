/*
 * server.cpp — NetHID TCP socket server with authentication
 *
 * Auth protocol (matches Python server.py):
 *   Binary first packet:  0xA0 <len:u8> <password>
 *     Reply: 0xA0 0x01 = ok, 0xA0 0x00 = wrong, 0xA0 0x02 = locked
 *   JSON first line:      {"type":"auth","password":"..."}
 *     Reply: {"ok":true} or {"error":"..."} 
 *
 * New commands added vs original:
 *   Binary 0x05: Wake  1 B  → assert USB Remote Wakeup
 *   JSON {"type":"wake"}    → same
 *   JSON {"type":"logout"}  → lock device
 *   JSON {"type":"status"}  → return auth status JSON
 */

#include "tusb.h"
#include "server.h"
#include "nethid.h"
#include "auth.h"
#include "secrets.h"
#include "config.h"
#include "net_compat.h"

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ── Minimal JSON helpers (no heap) ───────────────────────────────────────────

static int json_int(const char *json, const char *key, int def) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    for (;;) {                              /* match the key as an object key, not a value */
        p = strstr(p, pat); if (!p) return def;
        const char *q = p + strlen(pat);
        while (*q==' ') q++;
        if (*q==':') { p = q + 1; break; }  /* "key": ... -> this is the real key */
        p += strlen(pat);                   /* key text appeared as a value; keep looking */
    }
    while (*p==' ') p++;
    bool neg = (*p=='-'); if (neg) p++;
    if (*p<'0'||*p>'9') return def;
    int v=0; while (*p>='0'&&*p<='9') v=v*10+(*p++-'0');
    return neg?-v:v;
}

static size_t json_str(const char *json, const char *key, char *buf, size_t blen) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    for (;;) {                              /* match the key as an object key, not a value */
        p = strstr(p, pat); if (!p) return 0;
        const char *q = p + strlen(pat);
        while (*q==' ') q++;
        if (*q==':') { p = q + 1; break; }  /* "key": ... -> this is the real key */
        p += strlen(pat);                   /* key text appeared as a value; keep looking */
    }
    while (*p==' ') p++;
    if (*p++!='"') return 0;
    size_t n=0;
    while (*p && *p!='"' && n<blen-1) {
        if (*p=='\\') { p++; switch(*p){case 'n':buf[n++]='\n';break;case 't':buf[n++]='\t';break;default:buf[n++]=*p;} }
        else buf[n++]=*p;
        p++;
    }
    buf[n]='\0'; return n;
}

static void json_int_arr(const char *json, const char *key, int *arr, int count) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat); if (!p) return;
    p += strlen(pat);
    while (*p==' ') p++;
    if (*p++!=':') return;
    while (*p==' ') p++;
    if (*p++!='[') return;
    for (int i=0;i<count;i++){
        while (*p==' '||*p==',') p++;
        if (*p==']'||!*p) break;
        bool neg=(*p=='-'); if(neg)p++;
        if(*p>='0'&&*p<='9'){int v=0;while(*p>='0'&&*p<='9')v=v*10+(*p++-'0');arr[i]=neg?-v:v;}
    }
}

static inline int8_t clamp8(int v) { return v>127?127:v<-127?-127:(int8_t)v; }

// ── JSON dispatch (authenticated) ────────────────────────────────────────────

static void handle_json(const char *line, net_pcb *pcb, int user_idx) {
    char t[32] = {};
    json_str(line, "type", t, sizeof(t));

    const char *reply = "{\"ok\":true}\n";

    if (strcmp(t, "key") == 0) {
        int modifier=json_int(line,"modifier",0), keys[6]={};
        json_int_arr(line,"keys",keys,6);
        hid_keyboard_report_t rep = {};
        rep.modifier = (uint8_t)modifier;
        for (int i=0;i<6;i++) rep.keycode[i]=(uint8_t)keys[i];
        hid_push_key_report(&rep, false);

    } else if (strcmp(t, "key_release") == 0) {
        hid_push_key_release();

    } else if (strcmp(t, "mouse") == 0) {
        hid_mouse_report_t rep = {
            .buttons=(uint8_t)json_int(line,"buttons",0),
            .x=clamp8(json_int(line,"x",0)),
            .y=clamp8(json_int(line,"y",0)),
            .wheel=clamp8(json_int(line,"wheel",0)),.pan=0
        };
        hid_push_mouse_report(&rep);

    } else if (strcmp(t, "mouse_click") == 0) {
        int btn = json_int(line,"button",1);
        hid_mouse_report_t p={.buttons=(uint8_t)btn,.x=0,.y=0,.wheel=0,.pan=0}, r={};
        hid_push_mouse_report(&p);
        hid_push_mouse_report(&r);

    } else if (strcmp(t, "mouse_abs") == 0) {
        // Absolute position. x/y are 0..32767 over the full screen.
        int x = json_int(line,"x",0); if (x<0) x=0; if (x>HID_ABS_MAX) x=HID_ABS_MAX;
        int y = json_int(line,"y",0); if (y<0) y=0; if (y>HID_ABS_MAX) y=HID_ABS_MAX;
        hid_push_abs_pointer((uint16_t)x, (uint16_t)y,
                             (uint8_t)json_int(line,"buttons",0),
                             clamp8(json_int(line,"wheel",0)));

    } else if (strcmp(t, "text") == 0) {
        char text[256]={};
        size_t len = json_str(line,"text",text,sizeof(text));
        int delay = json_int(line,"delay_ms",TYPE_DELAY_MS);
        if (!len) { reply="{\"error\":\"bad params\"}\n"; goto send; }
        char expanded[256];
        const char *who = auth_user_name(user_idx);
        int elen = secret_expand(who, text, (int)len, expanded, sizeof(expanded));
        hid_push_type_string(expanded,(uint8_t)elen,(uint8_t)delay);

    } else if (strcmp(t, "combo") == 0) {
        hid_keyboard_report_t rep={};
        rep.modifier=(uint8_t)json_int(line,"modifier",0);
        rep.keycode[0]=(uint8_t)json_int(line,"key",0);
        hid_push_key_report(&rep, true);

    } else if (strcmp(t, "wake") == 0) {
        hid_push_wakeup();

    } else if (strcmp(t, "ping") == 0) {
        reply = "{\"type\":\"pong\"}\n";

    } else if (strcmp(t, "logout") == 0) {
        auth_logout();
        reply = "{\"ok\":true,\"msg\":\"logged_out\"}\n";

    } else if (strcmp(t, "status") == 0) {
        auth_status_t st; auth_get_status(&st);
        char wd[128]; hid_wake_diag(wd, sizeof(wd));
        static char sbuf[320];
        snprintf(sbuf, sizeof(sbuf),
                 "{\"authenticated\":%s,\"locked\":%s,"
                 "\"locked_for\":%u,\"idle_for\":%u,\"timeout\":%u,"
                 "\"suspended\":%s,\"wake_diag\":\"%s\"}\n",
                 st.authenticated?"true":"false",
                 st.locked?"true":"false",
                 (unsigned)st.locked_for_s,
                 (unsigned)st.idle_for_s,
                 (unsigned)st.timeout_s,
                 hid_is_suspended()?"true":"false",
                 wd);
        reply = sbuf;

    } else {
        reply = "{\"error\":\"unknown_type\"}\n";
    }

send:
    if (pcb) { net_write(pcb,reply,(u16_t)strlen(reply),TCP_WRITE_FLAG_COPY); net_output(pcb); }
}

// ── Binary dispatch (authenticated) ──────────────────────────────────────────

static int handle_binary(const uint8_t *buf, size_t len, net_pcb *pcb, int user_idx) {
    if (!len) return 0;
    uint8_t cmd = buf[0];

    if (cmd==0x01) {
        if (len<8) return 0;
        hid_keyboard_report_t rep={};
        rep.modifier=buf[1]; memcpy(rep.keycode,buf+3,6);
        hid_push_key_report(&rep,false); return 8;

    } else if (cmd==0x02) {
        if (len<5) return 0;
        hid_mouse_report_t rep={.buttons=buf[1],.x=(int8_t)buf[2],.y=(int8_t)buf[3],.wheel=(int8_t)buf[4],.pan=0};
        hid_push_mouse_report(&rep); return 5;

    } else if (cmd==0x03) {
        if (len<2) return 0;
        uint8_t slen=buf[1];
        if (len<(size_t)(2+slen)) return 0;
        char expanded[256];
        const char *who = auth_user_name(user_idx);
        int elen = secret_expand(who, (const char*)(buf+2), slen, expanded, sizeof(expanded));
        hid_push_type_string(expanded,(uint8_t)elen,TYPE_DELAY_MS); return 2+slen;

    } else if (cmd==0x04) {
        if (len<3) return 0;
        hid_keyboard_report_t rep={}; rep.modifier=buf[1]; rep.keycode[0]=buf[2];
        hid_push_key_report(&rep,true); return 3;

    } else if (cmd==0x05) {
        // Wake command
        hid_push_wakeup(); return 1;

    } else if (cmd==0x06) {
        // Absolute mouse: 0x06 <buttons> <x_lo> <x_hi> <y_lo> <y_hi> <wheel>
        // x/y are little-endian uint16 in 0..32767.
        if (len<7) return 0;
        uint16_t x = (uint16_t)(buf[2] | (buf[3]<<8));
        uint16_t y = (uint16_t)(buf[4] | (buf[5]<<8));
        hid_push_abs_pointer(x, y, buf[1], (int8_t)buf[6]);
        return 7;

    } else if (cmd==0xFF) {
        uint8_t pong[2]={0xFF,0x00};
        if (pcb) { net_write(pcb,pong,2,TCP_WRITE_FLAG_COPY); net_output(pcb); }
        return 1;
    }

    return -1; // unknown
}

// ── Per-client state ──────────────────────────────────────────────────────────

typedef struct {
    uint8_t  buf[TCP_RECV_BUF];
    uint16_t len;
    bool     is_json;
    bool     mode_decided;
    bool     authed;
    int8_t   user_idx;     // configured-user index once authed (0 = first user)
} client_t;

static client_t      clients[MAX_TCP_CLIENTS];
static net_pcb *cpcbs[MAX_TCP_CLIENTS];

static int  alloc_slot(net_pcb *p) {
    for (int i=0;i<MAX_TCP_CLIENTS;i++) {
        if (!cpcbs[i]) {
            cpcbs[i]=p;
            memset(&clients[i],0,sizeof(clients[i]));
            return i;
        }
    }
    return -1;
}
static void free_slot(net_pcb *p) {
    for(int i=0;i<MAX_TCP_CLIENTS;i++) if(cpcbs[i]==p){cpcbs[i]=nullptr;return;}
}
static client_t *get_client(net_pcb *p) {
    for (int i=0;i<MAX_TCP_CLIENTS;i++) {
        if (cpcbs[i]==p) return &clients[i];
    }
    return nullptr;
}

// ── lwIP callbacks ────────────────────────────────────────────────────────────

static void close_conn(net_pcb *pcb) {
    free_slot(pcb);
    net_arg(pcb,nullptr); net_recv(pcb,nullptr); net_err(pcb,nullptr);
    net_close(pcb);
    printf("[tcp] Client disconnected\n");
}

static err_t recv_cb(void *arg, net_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p || err!=ERR_OK) { close_conn(pcb); if(p) pbuf_free(p); return ERR_OK; }

    client_t *cs = get_client(pcb);
    if (!cs) { pbuf_free(p); return ERR_OK; }

    for (struct pbuf *q=p; q; q=q->next) {
        uint16_t space=TCP_RECV_BUF-cs->len, copy=q->len<space?q->len:space;
        memcpy(cs->buf+cs->len,q->payload,copy); cs->len+=copy;
    }
    net_recved(pcb,p->tot_len); pbuf_free(p);

    if (!cs->mode_decided && cs->len>0) {
        cs->is_json      = (cs->buf[0]=='{');
        cs->mode_decided = true;
        cs->authed       = !auth_is_enabled();
        cs->user_idx     = 0;     // default first user (used when auth disabled)
    }

    // Guard: if the buffer is full and we still can't make progress, the
    // client is sending an oversized line (JSON) or garbage (binary).
    // Drop the connection rather than stalling forever with space==0.
    if (cs->len >= TCP_RECV_BUF) {
        if (cs->is_json) {
            if (memchr(cs->buf, '\n', cs->len) == nullptr) {
                printf("[tcp] Oversized line, closing\n");
                close_conn(pcb);
                return ERR_OK;
            }
        } else {
            printf("[tcp] Oversized packet, closing\n");
            close_conn(pcb);
            return ERR_OK;
        }
    }

    if (cs->is_json) {
        while (true) {
            uint8_t *nl=(uint8_t*)memchr(cs->buf,'\n',cs->len);
            if (!nl) break;
            *nl='\0';
            const char *line=(char*)cs->buf;

            if (!cs->authed) {
                char t[16]={};
                json_str(line,"type",t,sizeof(t));

                if (strcmp(t,"challenge")==0) {
                    // Issue a one-time nonce; connection stays unauthenticated.
                    char nonce[AUTH_NONCE_HEX+1];
                    if (auth_make_challenge(nonce)) {
                        char nb[64];
                        int nl2=snprintf(nb,sizeof(nb),"{\"nonce\":\"%s\"}\n",nonce);
                        net_write(pcb,nb,(u16_t)nl2,TCP_WRITE_FLAG_COPY);
                    } else {
                        const char *bz="{\"error\":\"busy\"}\n";
                        net_write(pcb,bz,(u16_t)strlen(bz),TCP_WRITE_FLAG_COPY);
                    }
                    net_output(pcb);
                } else if (strcmp(t,"auth")==0) {
                    char user[64]={}, nonce[AUTH_NONCE_HEX+1]={}, resp[80]={};
                    json_str(line,"user",user,sizeof(user));
                    json_str(line,"nonce",nonce,sizeof(nonce));
                    json_str(line,"response",resp,sizeof(resp));
                    uint32_t retry=0;
                    auth_result_t r;
                    if (resp[0]) {
                        r = auth_respond(user,nonce,resp,&retry);
                    } else if (auth_plaintext_allowed()) {
                        char pw[64]={};
                        json_str(line,"password",pw,sizeof(pw));
                        r = auth_authenticate(user,pw,&retry);
                    } else {
                        const char *hr="{\"error\":\"hmac_required\"}\n";
                        net_write(pcb,hr,(u16_t)strlen(hr),TCP_WRITE_FLAG_COPY);
                        net_output(pcb); close_conn(pcb); return ERR_OK;
                    }
                    if (r==AUTH_OK || r==AUTH_DISABLED) {
                        cs->authed=true;
                        { int ui=auth_user_index(user); cs->user_idx=(int8_t)(ui<0?0:ui); }
                        net_write(pcb,"{\"ok\":true}\n",12,TCP_WRITE_FLAG_COPY);
                    } else if (r==AUTH_LOCKED) {
                        static char lbuf[64];
                        snprintf(lbuf,sizeof(lbuf),"{\"error\":\"locked\",\"retry_after\":%u}\n",(unsigned)retry);
                        net_write(pcb,lbuf,(u16_t)strlen(lbuf),TCP_WRITE_FLAG_COPY);
                        net_output(pcb); close_conn(pcb); return ERR_OK;
                    } else {
                        net_write(pcb,"{\"error\":\"wrong_password\"}\n",27,TCP_WRITE_FLAG_COPY);
                        net_output(pcb); close_conn(pcb); return ERR_OK;
                    }
                    net_output(pcb);
                } else {
                    net_write(pcb,"{\"error\":\"expected_auth\"}\n",26,TCP_WRITE_FLAG_COPY);
                    net_output(pcb); close_conn(pcb); return ERR_OK;
                }
            } else {
                if (!auth_check()) {
                    net_write(pcb,"{\"error\":\"session_expired\"}\n",28,TCP_WRITE_FLAG_COPY);
                    net_output(pcb); close_conn(pcb); return ERR_OK;
                }
                handle_json(line, pcb, cs->user_idx);
                auth_touch();
            }

            size_t used=(size_t)(nl-cs->buf)+1;
            cs->len-=(uint16_t)used;
            memmove(cs->buf,cs->buf+used,cs->len);
        }
    } else {
        while (cs->len>0) {
            if (!cs->authed) {
                if (!auth_plaintext_allowed()) {
                    // Binary password auth carries the password in the clear and
                    // has no HMAC variant — refuse it. Status 0x05 = hmac_required.
                    const uint8_t _r[]={0xA0,0x05}; net_write(pcb,_r,2,TCP_WRITE_FLAG_COPY);
                    net_output(pcb); close_conn(pcb); return ERR_OK;
                }
                if (cs->buf[0]!=0xA0) { do { const uint8_t _r[]={0xA0,0x03}; net_write(pcb,_r,2,TCP_WRITE_FLAG_COPY); } while(0); net_output(pcb); close_conn(pcb); return ERR_OK; }
                if (cs->len<2) break;
                uint8_t pwlen=cs->buf[1];
                if (cs->len<(uint16_t)(2+pwlen)) break;
                char pw[64]={};
                size_t cplen = pwlen<63?pwlen:63;
                memcpy(pw,cs->buf+2,cplen);
                cs->len-=(uint16_t)(2+pwlen);
                memmove(cs->buf,cs->buf+2+pwlen,cs->len);
                uint32_t retry=0;
                auth_result_t r=auth_authenticate(NULL,pw,&retry);
                if (r==AUTH_OK||r==AUTH_DISABLED) {
                    cs->authed=true;
                    cs->user_idx=0;   // binary auth has no user field -> first user
                    do { const uint8_t _r[]={0xA0,0x01}; net_write(pcb,_r,2,TCP_WRITE_FLAG_COPY); } while(0);
                } else if (r==AUTH_LOCKED) {
                    do { const uint8_t _r[]={0xA0,0x02}; net_write(pcb,_r,2,TCP_WRITE_FLAG_COPY); } while(0);
                    net_output(pcb); close_conn(pcb); return ERR_OK;
                } else {
                    do { const uint8_t _r[]={0xA0,0x00}; net_write(pcb,_r,2,TCP_WRITE_FLAG_COPY); } while(0);
                    net_output(pcb); close_conn(pcb); return ERR_OK;
                }
                net_output(pcb);
                continue;
            }
            if (!auth_check()) {
                do { const uint8_t _r[]={0xA0,0x04}; net_write(pcb,_r,2,TCP_WRITE_FLAG_COPY); } while(0);
                net_output(pcb); close_conn(pcb); return ERR_OK;
            }
            int consumed = handle_binary(cs->buf, cs->len, pcb, cs->user_idx);
            if (consumed==0) break;
            if (consumed<0)  { cs->len=0; break; }
            cs->len-=(uint16_t)consumed;
            memmove(cs->buf,cs->buf+consumed,cs->len);
            auth_touch();
        }
    }

    return ERR_OK;
}

static void err_cb(void *arg, err_t err) {
    (void)err;
    net_pcb *pcb=(net_pcb*)arg;
    if (pcb) free_slot(pcb);
    printf("[tcp] Client error\n");
}

static err_t accept_cb(void *arg, net_pcb *newpcb, err_t err) {
    (void)arg;
    if (err!=ERR_OK||!newpcb) return ERR_VAL;
    int slot=alloc_slot(newpcb);
    if (slot<0) { net_abort(newpcb); return ERR_ABRT; }
    printf("[tcp] Client connected (slot %d)\n", slot);
    net_setprio(newpcb,TCP_PRIO_MIN);
    net_arg(newpcb,newpcb);
    net_recv(newpcb,recv_cb);
    net_err(newpcb,err_cb);
    return ERR_OK;
}

// ── Public init ───────────────────────────────────────────────────────────────

void server_init(void) {
    memset(cpcbs,0,sizeof(cpcbs));
    memset(clients,0,sizeof(clients));

    // Plaintext control socket on :9000 — unchanged for nc / shell clients.
    net_pcb *lpcb = net_listen_plaintext(TCP_PORT, accept_cb, MAX_TCP_CLIENTS);
    if (!lpcb) { printf("[tcp] listen :%d failed\n", TCP_PORT); return; }
    printf("[tcp] Listening on port %d (auth %s)\n",
           TCP_PORT, auth_is_enabled()?"enabled":"DISABLED");

#if ENABLE_SOCKET_TLS
    // Same protocol over TLS on :9443, sharing the HTTPS certificate. Uses the
    // same accept_cb — TLS is transparent to the per-connection code.
    net_pcb *lpcb_tls = net_listen_tls(SOCKET_TLS_PORT, accept_cb, MAX_TCP_CLIENTS);
    if (!lpcb_tls) printf("[tcp] TLS listen :%d failed (%s)\n",
                          SOCKET_TLS_PORT, net_listen_fail ? net_listen_fail : "?");
    else printf("[tcp] TLS listening on port %d\n", SOCKET_TLS_PORT);
#endif
}
