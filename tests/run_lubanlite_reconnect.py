#!/usr/bin/env python3
"""Exercise actual READY radio/readback/reconnect functions with a fake HCI.

This proves scheduling, ownership and readback handling, not phone behavior.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
adapter = (root/'port/luban-lite/rtcp_bt_lubanlite.c').read_text()


def function(signature):
    match = re.search(re.escape(signature) + r'[^;{]*\{', adapter)
    assert match, signature
    start, end = match.start(), match.end()
    depth = 1
    while depth:
        depth += (adapter[end] == '{')-(adapter[end] == '}')
        end += 1
    return adapter[start:end]+'\n'


source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rtcp_lubanlite.h"
#include "rtcp/rtcp.h"
#define RTCP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define RTCP_LOGW(tag, ...) RTCP_LOGI(tag, __VA_ARGS__)
#define RTCP_LOGE(tag, ...) RTCP_LOGI(tag, __VA_ARGS__)
#define BT_TAG "test"
#define CONFIG_BT_MAX_PAIRED 2
#define BT_CONN_STATE_DISCONNECTED 0
#define BT_CONN_STATE_CONNECTING 1
#define BT_CONN_STATE_CONNECTED 2
#define BT_CONN_STATE_DISCONNECTING 3
#define BT_CONN_TYPE_BR 1
#define BT_CONN_ROLE_CENTRAL 0
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_SECURITY_L2 2
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define BT_BREDR_SCAN_PAGE 2
#define BT_BREDR_SCAN_INQUIRY 1
#define RTCP_BT_HCI_OP_READ_SCAN_ENABLE 0x0c19
#define BT_BR_CONN_PARAM_DEFAULT NULL
#define BT_LINK_MAX 1
#define BT_LINK_FREE 0
#define BT_LINK_CONNECTED 2
#define BT_LINK_DISCONNECTED 3
#define RT_EOK 0
typedef int rt_err_t;
typedef unsigned rt_base_t;
typedef struct { uint8_t val[6]; } bt_addr_t;
struct bt_conn_info { int state, role; };
struct bt_conn { int state, role, refs; bt_addr_t peer; };
struct bt_br_bond_info { bt_addr_t addr; };
struct net_buf { unsigned len; uint8_t *data; };
struct reconnect_bonds { bt_addr_t peers[CONFIG_BT_MAX_PAIRED]; unsigned count; };
enum bt_br_conn_req_rsp { BT_BR_CONN_REQ_ACCEPT_CENTRAL, BT_BR_CONN_REQ_ACCEPT_PERIPHERAL,
                         BT_BR_CONN_REQ_REJECT_NO_RESOURCES };
static bool s_stop, s_listening;
static struct bt_conn *s_reconnect_conn;
static bool s_reconnect_secure;
static uint32_t s_reconnect_started, s_reconnect_next, s_radio_checked;
static unsigned s_reconnect_cursor;
static rtcp_bt_pairing_status_t s_pair;
static bool s_bootstrap_peer_valid;
static bt_addr_t s_bootstrap_peer;
static struct bt_link { int state, accept_sem; bool handed_out; unsigned head, tail; } s_links[1];
static struct bt_conn connections[2];
static unsigned hardware_scan, host_scan, bonds, pages, security_calls, disconnects, reads, unrefs;
static int read_error, read_status, reply_size, write_error, security_error, sem_error;
static bool create_error, incoming_race, stop_during_read;
static uint32_t now;
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t flags) { (void)flags; }
static uint32_t rt_tick_get_millisecond(void) { return now; }
static unsigned rt_tick_from_millisecond(int ms) { return (unsigned)ms; }
static int rt_sem_take(int *sem, unsigned ticks) { (void)sem; assert(ticks<=500); return sem_error; }
bool rtcp_bt_lubanlite_link_up(void) { return s_links[0].state==BT_LINK_CONNECTED; }
int rtcp_bt_lubanlite_local_bdaddr(uint8_t address[6]) { memset(address,0x12,6); return RTCP_OK; }
static int bt_br_set_connectable(bool enabled,
        enum bt_br_conn_req_rsp (*callback)(const bt_addr_t *, uint32_t)) {
    (void)callback;
    if (enabled == !!(host_scan&2)) return -EALREADY;
    if (write_error) return write_error;
    host_scan=hardware_scan=enabled ? 2 : 0; return 0;
}
static int bt_br_set_discoverable(bool enabled, bool limited) {
    assert(enabled && !limited);
    if (host_scan&1) return -EALREADY;
    if (write_error) return write_error;
    host_scan=hardware_scan=3; return 0;
}
static int bt_hci_cmd_send_sync(unsigned opcode, void *input, struct net_buf **output) {
    assert(opcode==0x0c19 && !input); ++reads;
    if (stop_during_read) s_stop=true;
    static uint8_t data[2]; static struct net_buf result;
    data[0]=(uint8_t)read_status; data[1]=(uint8_t)hardware_scan;
    result.data=data; result.len=(unsigned)reply_size;
    *output=&result; return read_error;
}
static void net_buf_unref(struct net_buf *buf) { assert(buf); ++unrefs; }
static int bt_conn_get_info(struct bt_conn *conn, struct bt_conn_info *info) {
    assert(conn); info->state=conn->state; info->role=conn->role; return 0;
}
static const bt_addr_t *bt_conn_get_dst_br(struct bt_conn *conn) { return &conn->peer; }
static int bt_conn_disconnect(struct bt_conn *conn, unsigned reason) {
    assert(conn && reason==0x13); ++disconnects; conn->state=BT_CONN_STATE_DISCONNECTING; return 0;
}
static void bt_conn_unref(struct bt_conn *conn) { assert(conn->refs>0); --conn->refs; }
static int bt_conn_set_security(struct bt_conn *conn, unsigned level) {
    assert(conn && level==BT_SECURITY_L2); ++security_calls; return security_error;
}
static void bt_conn_foreach(unsigned type, void (*cb)(struct bt_conn *, void *), void *user) {
    assert(type==BT_CONN_TYPE_BR);
    for (unsigned i=0;i<2;++i) cb(&connections[i],user);
}
static void bt_br_foreach_bond(void (*cb)(const struct bt_br_bond_info *, void *), void *user) {
    for (unsigned i=0;i<bonds;++i) {
        struct bt_br_bond_info info={.addr={{(uint8_t)i,2,3,4,5,6}}}; cb(&info,user);
    }
}
static struct bt_conn *bt_conn_create_br(const bt_addr_t *peer, const void *param) {
    assert(!param && peer->val[0]<2); ++pages;
    if (create_error) return NULL;
    struct bt_conn *conn=&connections[peer->val[0]];
    conn->peer=*peer;
    conn->state=BT_CONN_STATE_CONNECTING;
    conn->role=incoming_race ? BT_CONN_ROLE_PERIPHERAL : BT_CONN_ROLE_CENTRAL;
    ++conn->refs; return conn;
}
'''
for signature in (
    'static enum bt_br_conn_req_rsp ready_connection_request(',
    'static int publish_service(', 'static int read_scan_enable(',
    'static int verify_ready_radio(', 'static void collect_reconnect_bond(',
    'static void count_active_acl(', 'static void release_reconnect(',
    'int rtcp_bt_lubanlite_reconnect_tick(', 'static int publish_ready_service(',
    'int rtcp_bt_lubanlite_rfcomm_accept_timeout(',
):
    source += function(signature)
source += r'''
static void reset(void) {
    assert(!s_reconnect_conn);
    for (unsigned i=0;i<2;++i) assert(!connections[i].refs);
    memset(connections,0,sizeof(connections)); memset(s_links,0,sizeof(s_links));
    memset(&s_pair,0,sizeof(s_pair));
    hardware_scan=host_scan=bonds=pages=security_calls=disconnects=reads=unrefs=0;
    read_error=read_status=write_error=security_error=0; reply_size=2;
    create_error=incoming_race=stop_during_read=false; sem_error=-1;
    s_listening=true; s_stop=false; now=0; s_reconnect_cursor=0;
    s_bootstrap_peer_valid=false;
}
int main(void) {
    reset(); assert(!publish_ready_service());
    assert(hardware_scan==3 && s_pair.scan_known && !s_pair.radio_error && reads==unrefs);
    now=2000; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && !pages); /* no bond */

    reset(); host_scan=3; hardware_scan=0;
    assert(!publish_ready_service() && hardware_scan==3 && reads==2); /* stale host flags */
    hardware_scan=0; now=10000;
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && hardware_scan==3); /* idle repair */

    reset(); read_error=-EIO;
    assert(publish_ready_service()==RTCP_EIO && s_stop && !s_pair.scan_known && reads==unrefs);
    reset(); reply_size=1;
    assert(publish_ready_service()==RTCP_EIO && s_stop);
    reset(); read_status=1;
    assert(publish_ready_service()==RTCP_EIO && s_stop);
    reset(); stop_during_read=true;
    assert(publish_ready_service()==RTCP_ESTATE && s_stop && !hardware_scan);
    reset(); host_scan=3; write_error=-EIO;
    assert(publish_ready_service()==RTCP_EIO && s_stop);

    reset(); bonds=2; assert(!publish_ready_service());
    now=1999; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && !pages);
    now=2000; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && pages==1);
    assert(s_reconnect_conn==&connections[0] && connections[0].refs==1);
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && !security_calls);
    connections[0].state=BT_CONN_STATE_CONNECTED;
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && security_calls==1);
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && security_calls==1);
    now=22000; assert(!rtcp_bt_lubanlite_reconnect_tick(true));
    assert(!s_reconnect_conn && disconnects==1 && !connections[0].refs && bonds==2);
    connections[0].state=BT_CONN_STATE_DISCONNECTED;
    now=31999; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && pages==1);
    now=32000; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && pages==2);
    assert(s_reconnect_conn==&connections[1]); /* rotate saved phones */
    s_links[0].state=BT_LINK_CONNECTED;
    s_bootstrap_peer=connections[1].peer; s_bootstrap_peer_valid=true;
    assert(!rtcp_bt_lubanlite_reconnect_tick(false) && !s_reconnect_conn && disconnects==1);
    now+=60000; assert(!rtcp_bt_lubanlite_reconnect_tick(false) && pages==2); /* Wi-Fi handoff */

    reset(); bonds=1; assert(!publish_ready_service());
    now=2000; assert(!rtcp_bt_lubanlite_reconnect_tick(true));
    assert(!rtcp_bt_lubanlite_reconnect_tick(false) && disconnects==1 && !s_reconnect_conn);
    assert(!connections[0].refs); /* stop/Wi-Fi cancels only our pending page */

    reset(); bonds=1; assert(!publish_ready_service());
    now=2000; assert(!rtcp_bt_lubanlite_reconnect_tick(true));
    s_links[0].state=BT_LINK_CONNECTED; s_bootstrap_peer_valid=true;
    memset(&s_bootstrap_peer,0x55,sizeof(s_bootstrap_peer)); /* another phone won */
    assert(!rtcp_bt_lubanlite_reconnect_tick(false) && disconnects==1 && !s_reconnect_conn);

    reset(); bonds=1; assert(!publish_ready_service());
    connections[1].state=BT_CONN_STATE_CONNECTING; connections[1].role=BT_CONN_ROLE_PERIPHERAL;
    now=2000; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && !pages); /* incoming wins */
    connections[1].state=BT_CONN_STATE_DISCONNECTED; incoming_race=true;
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && pages==1 && !s_reconnect_conn);
    assert(!connections[0].refs && !disconnects); /* never cancel the incoming ACL */

    reset(); bonds=1; assert(!publish_ready_service());
    now=2000; assert(!rtcp_bt_lubanlite_reconnect_tick(true));
    connections[0].state=BT_CONN_STATE_DISCONNECTED; /* page/auth failure callback */
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && !s_reconnect_conn && !connections[0].refs);
    now=32000; create_error=true;
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && pages==2 && !s_reconnect_conn);

    reset(); bonds=1; assert(!publish_ready_service());
    now=2000; assert(!rtcp_bt_lubanlite_reconnect_tick(true));
    connections[0].state=BT_CONN_STATE_CONNECTED; security_error=-EIO;
    assert(!rtcp_bt_lubanlite_reconnect_tick(true) && disconnects==1 && !s_reconnect_conn);

    reset(); bonds=1; now=0xffffff00U; assert(!publish_ready_service());
    now+=1999; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && !pages);
    ++now; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && pages==1);
    s_stop=true; assert(!rtcp_bt_lubanlite_reconnect_tick(true) && disconnects==1 && !s_reconnect_conn);

    reset(); int fd=-1;
    assert(rtcp_bt_lubanlite_rfcomm_accept_timeout(1,&fd,500)==RTCP_ETIMEDOUT);
    sem_error=0; s_links[0].state=BT_LINK_CONNECTED;
    assert(!rtcp_bt_lubanlite_rfcomm_accept_timeout(1,&fd,500) && fd==1 && s_links[0].handed_out);
    s_links[0].state=BT_LINK_DISCONNECTED;
    assert(rtcp_bt_lubanlite_rfcomm_accept_timeout(1,&fd,0)==RTCP_ETIMEDOUT && s_links[0].state==BT_LINK_FREE);
    s_stop=true; assert(rtcp_bt_lubanlite_rfcomm_accept_timeout(1,&fd,500)==RTCP_EIO);
    puts("Actual READY reconnect: HCI readback/repair/failure, bounded accept, bond paging/rotation/backoff, encryption, incoming race, stop/handoff, references and clock wrap passed");
}
'''
with tempfile.TemporaryDirectory(prefix='rtcp-ready-') as directory:
    c = Path(directory)/'test.c'
    binary = Path(directory)/'test'
    c.write_text(source)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), '-I'+str(root/'port/luban-lite/include'),
                    str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
