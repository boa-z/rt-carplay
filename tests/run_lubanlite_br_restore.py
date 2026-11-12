#!/usr/bin/env python3
"""Restore a cold-boot bond with the SDK's actual keys_br.c and key codec.

Only controller/RTOS/logging boundaries are mocked. No radio is exercised.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = Path(os.environ['SDK_ROOT']).resolve()
bt = sdk / 'packages/third-party/zephyr-bluetooth'
host = bt / 'subsys/bluetooth/host'


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


prefix = (root / 'tests/unit/test_lubanlite_bt_settings.c').read_text()
source = prefix[:prefix.index('static unsigned loaded')]
source += r'''
#define CONFIG_BT_SETTINGS 1
#define CONFIG_BT_MAX_PAIRED 4
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_MODULE_REGISTER(...)
#undef __ASSERT_NO_MSG
#define __ASSERT_NO_MSG(x) assert(x)
typedef struct { uint8_t val[6]; } bt_addr_t;
typedef struct { uint8_t type; bt_addr_t a; } bt_addr_le_t;
#define BT_ADDR_LE_PUBLIC 0
#define BT_ADDR_LE_RANDOM 1
static const bt_addr_t empty_addr;
#define BT_ADDR_ANY (&empty_addr)
static bool bt_addr_eq(const bt_addr_t *a, const bt_addr_t *b) { return !memcmp(a,b,6); }
static void bt_addr_copy(bt_addr_t *a, const bt_addr_t *b) { memcpy(a,b,6); }
struct bt_br_bond_info { bt_addr_t addr; };
'''
keys_header = (host / 'keys.h').read_text()
start = keys_header.index('struct bt_keys_link_key {')
end = keys_header.index('struct bt_keys_link_key *bt_keys_get_link_key', start)
source += keys_header[start:end]
source += 'void bt_keys_link_key_clear(struct bt_keys_link_key *key);\n'
settings = (host / 'settings.c').read_text(errors='replace')
# Use the same non-printf key encoding as the default SDK configuration.
source += function(settings[settings.index('#else /* !CONFIG_BT_SETTINGS_USE_PRINTK */'):],
                   'int bt_settings_encode_key(')
source += function(settings, 'int bt_settings_decode_key(')
source += r'''
static int bt_settings_store_link_key(const bt_addr_le_t *addr, const void *v, size_t n) {
    char path[64]; assert(!bt_settings_encode_key(path,sizeof(path),"link_key",addr,NULL));
    return settings_save_one(path,v,n);
}
static int bt_settings_delete_link_key(const bt_addr_le_t *addr) {
    char path[64]; assert(!bt_settings_encode_key(path,sizeof(path),"link_key",addr,NULL));
    return settings_delete(path);
}
'''
source += '\n'.join(line for line in (host / 'classic/keys_br.c').read_text().splitlines()
                    if not line.startswith('#include'))
source += r'''
#define BT_LINK_KEY_AUTHENTICATED 1
#define BT_LINK_KEY_SC 4
#define BT_SECURITY_L2 2
#define BT_HCI_ERR_AUTH_FAIL 5
#define BT_CONN_BR_LEGACY_SECURE 0
#define BT_CONN_BR_NOBOND 1
#ifndef CONFIG_BT_SMP_SC_ONLY
#define CONFIG_BT_SMP_SC_ONLY 0
#endif
struct bt_conn { struct { struct bt_keys_link_key *link_key; } br; unsigned required_sec_level; unsigned flags[1]; };
struct bt_hci_evt_link_key_req { bt_addr_t bdaddr; };
struct bt_hci_evt_link_key_notify { bt_addr_t bdaddr; uint8_t link_key[16], key_type; };
struct net_buf { void *data; };
static struct bt_conn connection;
static int replies, negatives, references, disconnected;
static bool atomic_test_bit(const unsigned *flags, unsigned bit) { return !!(*flags & (1U<<bit)); }
static bool atomic_test_and_clear_bit(unsigned *flags, unsigned bit) {
    bool set=atomic_test_bit(flags,bit); *flags &= ~(1U<<bit); return set;
}
static void ssp_pairing_complete(struct bt_conn *conn, unsigned error) { assert(conn && error); }
static unsigned bt_security_err_get(unsigned error) { return error; }
static int bt_conn_disconnect(struct bt_conn *conn, unsigned error) {
    assert(conn && error==BT_HCI_ERR_AUTH_FAIL); ++disconnected; return 0;
}
static uint8_t reply_key[16];
static struct bt_conn *bt_conn_lookup_addr_br(const bt_addr_t *addr) {
    assert(addr); ++references; return &connection;
}
static void bt_conn_unref(struct bt_conn *conn) { assert(conn==&connection); --references; }
static void link_key_neg_reply(const bt_addr_t *addr) { assert(addr); ++negatives; }
static void link_key_reply(const bt_addr_t *addr, const uint8_t *key) {
    assert(addr); memcpy(reply_key,key,16); ++replies;
}
'''
source += '\n'.join(line for line in (bt/'include/zephyr/bluetooth/hci_types.h').read_text().splitlines()
                    if line.startswith('#define BT_LK_')) + '\n'
source += function((host/'classic/ssp.c').read_text(), 'void bt_hci_link_key_req(')
source += function((host/'classic/ssp.c').read_text(), 'void bt_hci_link_key_notify(')
source += r'''
static unsigned bonds;
static void count(const struct bt_br_bond_info *info, void *arg) {
    (void)arg; assert(info); ++bonds;
}
static int boot(void) {
    memset(key_pool,0,sizeof(key_pool));
    memset(s_entries,0,sizeof(s_entries));
    s_ready=s_file_loaded=s_needs_compact=false; s_appends=0; s_storage_error=0;
    int rc=settings_subsys_init(); return rc ? rc : settings_load();
}
int main(void) {
    char directory[]="/tmp/rt-carplay-real-br-XXXXXX"; assert(mkdtemp(directory));
    snprintf(store,sizeof(store),"%s/bonds",directory);
    snprintf(temporary,sizeof(temporary),"%s/bonds.tmp",directory);
    snprintf(backup,sizeof(backup),"%s/bonds.bak",directory);
    assert(!boot());
    const bt_addr_t phone={{0x03,0xdd,0x73,0x4f,0xb5,0xdc}};
    struct bt_hci_evt_link_key_notify initial={.bdaddr=phone, .key_type=BT_LK_AUTH_COMBINATION_P256};
    for (unsigned i=0;i<16;++i) initial.link_key[i]=(uint8_t)(i+10);
    struct net_buf initial_packet={&initial}; bt_hci_link_key_notify(&initial_packet);
    assert(!references && !disconnected && !rtcp_bt_settings_error());
    struct bt_keys_link_key *key=bt_keys_find_link_key(&phone); assert(key && key->flags==5);
    for (unsigned i=0;i<3;++i) {
        assert(!boot()); bonds=0; bt_br_foreach_bond(count,NULL); assert(bonds==1);
        key=bt_keys_find_link_key(&phone); assert(key && key->flags==5);
        for (unsigned j=0;j<16;++j) assert(key->val[j]==j+10);
        memset(&connection,0,sizeof(connection)); connection.required_sec_level=3;
        struct bt_hci_evt_link_key_req request={phone}; struct net_buf packet={&request};
        int previous=replies; bt_hci_link_key_req(&packet);
        assert(replies==previous+1 && !negatives && !references);
        assert(!memcmp(reply_key,key->val,16));
    }
    /* A controller key refresh must persist the new key without losing
     * authentication/SC flags, then survive another cold boot. */
    struct bt_hci_evt_link_key_notify update={.bdaddr=phone, .key_type=BT_LK_CHANGED_COMBINATION};
    for (unsigned j=0;j<16;++j) update.link_key[j]=(uint8_t)(j+40);
    struct net_buf notification={&update};
    bt_hci_link_key_notify(&notification); assert(!references && !disconnected);
    assert(!boot()); key=bt_keys_find_link_key(&phone); assert(key && key->flags==5);
    assert(!memcmp(key->val,update.link_key,16));
    memset(&connection,0,sizeof(connection)); connection.required_sec_level=3;
    struct bt_hci_evt_link_key_req refreshed={phone}; struct net_buf refreshed_packet={&refreshed};
    bt_hci_link_key_req(&refreshed_packet);
    assert(!memcmp(reply_key,update.link_key,16) && !references);
    /* Unknown event types must not overwrite a valid stored key. */
    update.key_type=0xff; bt_hci_link_key_notify(&notification);
    assert(!references && disconnected==1);
    assert(!boot()); key=bt_keys_find_link_key(&phone);
    assert(key && key->flags==5 && !memcmp(key->val,update.link_key,16));
    /* An update without an original bond cannot invent security flags. */
    const bt_addr_t other={{1,2,3,4,5,6}};
    connection.br.link_key=NULL; update.bdaddr=other; update.key_type=BT_LK_CHANGED_COMBINATION;
    bt_hci_link_key_notify(&notification);
    assert(!references && disconnected==2 && !bt_keys_find_link_key(&other));
    /* Never weaken the requested security or invent a key when missing. */
    key->flags=0; connection.br.link_key=NULL; connection.required_sec_level=3;
    struct bt_hci_evt_link_key_req request={phone}; struct net_buf packet={&request};
    bt_hci_link_key_req(&packet); assert(negatives==1 && !references);
    memset(key_pool,0,sizeof(key_pool)); connection.br.link_key=NULL;
    bt_hci_link_key_req(&packet); assert(negatives==2 && !references);
    const char *path="bt/link_key/dcb54f73dd030";
    unsigned char incompatible[BT_KEYS_LINK_KEY_STORAGE_LEN+1]={0};
    for (unsigned n=BT_KEYS_LINK_KEY_STORAGE_LEN-1; n<=sizeof(incompatible); n+=2) {
        assert(!settings_save_one(path,incompatible,n));
        assert(boot()==-EINVAL); /* incompatible layout is not a tombstone */
        assert(settings_get_val_len(path)==n);
        assert(!bt_keys_find_link_key(&phone));
    }
    assert(!settings_delete(path)); assert(!boot());
    bonds=0; bt_br_foreach_bond(count,NULL); assert(!bonds);
    unlink(store); unlink(temporary); unlink(backup); assert(!rmdir(directory));
    puts("Real SDK BR: cold boot + HCI key reply; changed combination key persists with security flags; unknown/missing-key updates rejected; incompatible records preserved; deletion passed");
}
'''
with tempfile.TemporaryDirectory(prefix='rtcp-br-restore-') as directory:
    c = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    # The shared harness includes the backend relative to tests/unit/.
    source = source.replace('../../port/luban-lite/rtcp_bt_settings.c',
                            str(root / 'port/luban-lite/rtcp_bt_settings.c'))
    c.write_text(source)
    command = [os.environ.get('CC', 'cc'), '-std=c11', '-g', '-Wall', '-Wextra',
                    '-Werror', '-Wno-sign-compare', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-DCONFIG_X86', '-DCONFIG_64BIT', '-DRT_IPC_FLAG_FIFO=0',
                    '-I'+str(root/'include'), '-I'+str(root/'tests/support/lubanlite_demo'),
                    '-I'+str(bt/'include'), '-I'+str(bt/'porting'), str(c),
                    '-Wl,-T,'+str(root/'tests/support/lubanlite_demo/settings_sections.ld'),
                    '-o', str(binary)]
    for sc_only in (0, 1):
        subprocess.run(command + ['-DCONFIG_BT_SMP_SC_ONLY='+str(sc_only)], check=True)
        subprocess.run([str(binary)], check=True)
