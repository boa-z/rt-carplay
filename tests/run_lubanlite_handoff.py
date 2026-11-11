#!/usr/bin/env python3
"""Host-only lifecycle regression using the actual adapter/owner functions."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include "rtcp/rtcp.h"
#define IAP2_LOG(...) ((void)0)
#define RTCP_LOGI(...) ((void)0)
#define BT_TAG "test"
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define BT_CONN_STATE_CONNECTED 1
#define BT_CONN_STATE_DISCONNECTING 2
#define BT_CONN_STATE_DISCONNECTED 3
typedef unsigned rt_base_t;
typedef struct { unsigned value; } bt_addr_t;
struct bt_conn { unsigned value; };
struct bt_conn_info { int state; };
static bt_addr_t s_bootstrap_peer={42};
static bool s_bootstrap_peer_valid=true;
static unsigned s_bootstrap_generation=1;
static struct bt_conn connection;
static int state, disconnect_result, lookups, refs, disconnects;
static bool exists;
static uint64_t now;
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t irq) { (void)irq; }
static struct bt_conn *bt_conn_lookup_addr_br(const bt_addr_t *p) {
    assert(p->value==42); ++lookups;
    if (!exists) return NULL;
    ++refs; return &connection;
}
static int bt_conn_get_info(struct bt_conn *c, struct bt_conn_info *info) {
    assert(c==&connection); info->state=state; return 0;
}
static int bt_conn_disconnect(struct bt_conn *c, unsigned reason) {
    assert(c==&connection && reason==0x13); ++disconnects;
    if (!disconnect_result) state=BT_CONN_STATE_DISCONNECTING;
    return disconnect_result;
}
static void bt_conn_unref(struct bt_conn *c) { assert(c==&connection); --refs; }
static uint64_t now_ms(void) { return now; }
static const struct {uint64_t (*now_ms)(void);} rtcp_os_rtthread={now_ms};
static struct { void *session; bool authenticated, identification_accepted; int error; } s_tunnel;
static bool s_handoff_requested, s_handoff_done;
static unsigned s_handoff_generation;
static uint64_t s_handoff_at_ms;
static void iap2_session_advance(void *session, uint64_t at) { assert(session && at==now); }
'''
source += function('port/luban-lite/rtcp_bt_lubanlite.c', 'unsigned rtcp_bt_lubanlite_bootstrap_generation(void)')
source += function('port/luban-lite/rtcp_bt_lubanlite.c', 'int rtcp_bt_lubanlite_handoff(unsigned generation)')
source += function('examples/rtthread_app/iap2_link_lubanlite.c', 'bool carplay_iap2_tunnel_tick(void *context)')
source += function('examples/rtthread_app/iap2_link_lubanlite.c', 'bool carplay_iap2_request_handoff(const char *device, void *context)')
source += r'''
static void reset(void) {
    state=BT_CONN_STATE_CONNECTED; disconnect_result=lookups=refs=disconnects=0;
    exists=true; s_handoff_requested=s_handoff_done=false; s_handoff_at_ms=0;
    s_bootstrap_generation=1; now=100;
    s_tunnel.session=&connection; s_tunnel.authenticated=false;
    s_tunnel.identification_accepted=false; s_tunnel.error=0;
}
int main(void) {
    reset();
    assert(carplay_iap2_request_handoff("phone",NULL));
    assert(carplay_iap2_tunnel_tick(NULL) && lookups==0);
    s_tunnel.authenticated=true;
    assert(carplay_iap2_tunnel_tick(NULL) && lookups==0); /* identity still absent */
    s_tunnel.identification_accepted=true;
    assert(carplay_iap2_tunnel_tick(NULL) && disconnects==1 && !s_handoff_done && refs==0);
    now=200;
    assert(carplay_iap2_tunnel_tick(NULL) && disconnects==1 && !s_handoff_done && refs==0);
    state=BT_CONN_STATE_DISCONNECTED;
    assert(carplay_iap2_tunnel_tick(NULL) && s_handoff_done && refs==0);
    int previous=lookups;
    assert(carplay_iap2_tunnel_tick(NULL) && lookups==previous);

    reset(); carplay_iap2_request_handoff("phone",NULL);
    ++s_bootstrap_generation; /* a replacement bootstrap must not be closed */
    s_tunnel.authenticated=s_tunnel.identification_accepted=true;
    assert(carplay_iap2_tunnel_tick(NULL) && !s_handoff_done && !s_handoff_requested);
    assert(!lookups && !disconnects && !refs);

    reset(); carplay_iap2_request_handoff("phone",NULL);
    s_tunnel.authenticated=s_tunnel.identification_accepted=true;
    assert(carplay_iap2_tunnel_tick(NULL)); now=3101;
    assert(carplay_iap2_tunnel_tick(NULL) && !s_handoff_done && !s_handoff_requested && !refs);
    assert(disconnects==1); /* no disconnect flood while waiting for HCI */

    reset(); carplay_iap2_request_handoff("phone",NULL);
    s_tunnel.authenticated=s_tunnel.identification_accepted=true;
    disconnect_result=-EIO;
    assert(carplay_iap2_tunnel_tick(NULL) && !s_handoff_done && !s_handoff_requested && !refs);

    reset(); carplay_iap2_request_handoff("phone",NULL);
    s_tunnel.authenticated=s_tunnel.identification_accepted=true; exists=false;
    assert(carplay_iap2_tunnel_tick(NULL) && s_handoff_done && !disconnects && !refs);
    s_tunnel.error=RTCP_EIO;
    assert(!carplay_iap2_tunnel_tick(NULL));
    puts("Actual wireless handoff lifecycle: auth/identity gate, async HCI close, generation, timeout, failure and references passed");
}
'''
with tempfile.TemporaryDirectory(prefix='rtcp-handoff-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(source)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-I'+str(root/'include'),
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
