/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define RTCP_USB_TRACE_IN_TIMEOUTS 1
/* Compile the actual SDK port, including its private DMA submission helpers. */
#include "../../port/luban-lite/rtcp_usb_lubanlite.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int allocations, semaphores, locked, fail_alloc, fail_sem;
static int submit_error, timeout, kill_error, short_by, extra, kills;
static struct usbh_urb *pending;
static struct usb_dma_transfer *allocation;
static struct usb_bulk_state pipe;
static unsigned peer_toggle[32], delivered[32];
static int simulate_pid, disconnect_in_submit;
static unsigned last_pid;
static int diagnostic_completes, diagnostic_length, diagnostic_error, diagnostic_disconnect;
static int kill_completes;
void usbh_ehci_transfer_snapshot(struct usbh_urb *u, int reason)
{
    assert(reason == -USB_ERR_TIMEOUT && u == pending);
    if (diagnostic_completes) {
        u->errorcode = diagnostic_error;
        u->actual_length = (unsigned)diagnostic_length;
        if (diagnostic_length) memset(u->transfer_buffer, 0xa6, (size_t)diagnostic_length);
        u->data_toggle ^= 1U;
        if (diagnostic_disconnect) { ++pipe.generation; pipe.data_toggle = 0; }
        u->complete(u->arg, diagnostic_error ? diagnostic_error : diagnostic_length);
    }
}

static int critical;
size_t usb_osal_enter_critical_section(void) { assert(!critical); critical = 1; return 0; }
void usb_osal_leave_critical_section(size_t flags) { (void)flags; assert(critical); critical = 0; }
void *rt_malloc_align(size_t n, size_t align)
{
    if (fail_alloc) return NULL;
    assert(n % align == 0);
    void *p; assert(!posix_memalign(&p, align, n));
    allocations++; allocation = p; return p;
}
void rt_free_align(void *p) { allocations--; free(p); }
usb_osal_sem_t usb_osal_sem_create(uint32_t initial)
{
    assert(!initial);
    if (fail_sem) return NULL;
    int *s = calloc(1, sizeof(*s)); assert(s); semaphores++; return s;
}
void usb_osal_sem_delete(usb_osal_sem_t s) { semaphores--; free(s); }
int usb_osal_sem_give(usb_osal_sem_t s) { *(int *)s = 1; return 0; }
int usb_osal_sem_take(usb_osal_sem_t s, uint32_t ms)
{ assert(ms); return *(int *)s ? 0 : -USB_ERR_TIMEOUT; }
int usb_osal_mutex_take(usb_osal_mutex_t m)
{ assert(m && !locked); locked = 1; return 0; }
int usb_osal_mutex_give(usb_osal_mutex_t m)
{ assert(m && locked); locked = 0; return 0; }
int usbh_submit_urb(struct usbh_urb *u)
{
    assert(locked);
    assert((uintptr_t)u->setup % 64 == 0);
    assert((uintptr_t)u->transfer_buffer % 64 == 0);
    assert(u == &allocation->urb && !u->timeout);
    if (submit_error) return submit_error;
    pending = u;
    if (timeout) return 0;
    if (simulate_pid && !u->setup) {
        unsigned index = (u->ep->bEndpointAddress & 15U) +
                         ((u->ep->bEndpointAddress & 0x80U) ? 16U : 0U);
        unsigned mps = u->ep->wMaxPacketSize;
        assert(mps);
        last_pid = u->data_toggle;
        /* One successful short packet, full packets, and a ZLP all advance
         * the receiver's expected DATA PID. Duplicate OUT is ACKed but not
         * delivered to its consumer, exactly why USB success isn't enough. */
        unsigned packets = (u->transfer_buffer_length + mps - 1U) / mps;
        if (!packets) packets = 1;
        if (peer_toggle[index] == u->data_toggle) {
            ++delivered[index];
            peer_toggle[index] ^= packets & 1U;
        }
        u->data_toggle ^= packets & 1U;
    }
    if (disconnect_in_submit) { ++pipe.generation; pipe.data_toggle = 0; }
    int len = (int)u->transfer_buffer_length;
    bool input = u->setup ? (u->setup->bmRequestType & 0x80) != 0
                         : (u->ep->bEndpointAddress & 0x80) != 0;
    if (input && len) memset(u->transfer_buffer, 0xa5, (size_t)len);
    if (!input) for (int i = 0; i < len; ++i) assert(u->transfer_buffer[i] == 0x37);
    u->actual_length = len - short_by + extra + (u->setup ? 8 : 0);
    u->complete(u->arg, (int)u->actual_length);
    return 0;
}
int usbh_kill_urb(struct usbh_urb *u)
{
    assert(u == pending); kills++;
    if (kill_completes) {
        u->errorcode = 0; u->actual_length = 3;
        memset(u->transfer_buffer, 0xa7, 3);
        u->data_toggle ^= 1U;
        u->complete(u->arg, 3);
        return -USB_ERR_INVAL; /* completion already released hcpriv */
    }
    if (kill_error) return kill_error;
    u->complete(u->arg, -USB_ERR_SHUTDOWN); return 0;
}
static void clean(void) { assert(!allocations && !semaphores && !locked); }
int main(void)
{
    struct usbh_hubport h = {0}; h.mutex = &h;
    struct usb_endpoint_descriptor ep = {.bEndpointAddress = 0x85};
    uint8_t bytes[200]; uint8_t *unaligned = bytes + 1;
    memset(bytes, 0x37, sizeof(bytes));
    assert(control_transfer_timeout(&h, 0x80, 6, 0, 0, unaligned, 9, 100) == 9);
    assert(bytes[0] == 0x37 && bytes[10] == 0x37 && unaligned[0] == 0xa5); clean();
    assert(control_transfer_timeout(&h, 0x40, 4, 0, 0, NULL, 0, 100) == 0); clean();
    short_by = 3;
    memset(bytes, 0x37, sizeof(bytes));
    assert(submit_bulk(&h, &ep, unaligned, 65, 100, NULL) == 62);
    assert(unaligned[61] == 0xa5 && unaligned[62] == 0x37); clean();
    short_by = 0; ep.bEndpointAddress = 4;
    memset(bytes, 0x37, sizeof(bytes));
    assert(submit_bulk(&h, &ep, unaligned, 129, 100, NULL) == 129); clean();
    assert(submit_bulk(&h, &ep, NULL, 0, 100, NULL) == 0); clean();
    extra = 1;
    assert(control_transfer_timeout(&h, 0x80, 6, 0, 0, unaligned, 9, 100) == RTCP_EIO); clean();
    extra = 0; short_by = 10;
    assert(control_transfer_timeout(&h, 0x80, 6, 0, 0, unaligned, 9, 100) == RTCP_EIO); clean();
    short_by = 0;
    assert(submit_bulk(&h, &ep, NULL, 1, 100, NULL) == RTCP_EINVAL); clean();
    assert(submit_bulk(&h, &ep, unaligned, SIZE_MAX, 100, NULL) == RTCP_EINVAL); clean();
    fail_alloc = 1;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, NULL) == RTCP_ENOMEM); clean();
    fail_alloc = 0; fail_sem = 1;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, NULL) == RTCP_ENOMEM); clean();
    fail_sem = 0; submit_error = -USB_ERR_STALL;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, NULL) == RTCP_EIO); clean();
    submit_error = 0; timeout = 1;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, NULL) == RTCP_ETIMEDOUT); clean();
    assert(kills == 1); kill_error = -USB_ERR_TIMEOUT;
    assert(control_transfer_timeout(&h, 0x80, 6, 0, 0, unaligned, 9, 100) == RTCP_ETIMEDOUT);
    assert(allocations == 1 && semaphores == 1 && !locked);
    /* A late callback and DMA write remain valid after the caller returns. */
    pending->transfer_buffer[0] = 0xff;
    pending->complete(pending->arg, 1);
    usb_osal_sem_delete(allocation->done); rt_free_align(allocation); clean();
    /* Real owned-URB path: model VERSION, SETUP and SYN as three short OUT
     * transfers. A fresh zeroed URB each time must still use DATA0/1/0. */
    timeout = 0; kill_error = 0;
    simulate_pid = 1;
    ep.bEndpointAddress = 4; ep.wMaxPacketSize = 512;
    memset(bytes, 0x37, sizeof(bytes));
    const size_t lengths[] = {20, 17, 36};
    for (unsigned i = 0; i < 3; ++i) {
        assert(submit_bulk(&h, &ep, unaligned, lengths[i], 100, &pipe) == (int)lengths[i]);
        assert(last_pid == (i & 1U));
        assert(delivered[4] == i + 1U); clean();
    }
    /* IN and OUT maintain separate state. Repeated IN short replies carry
     * the HCD's returned toggle into the next allocated URB too. */
    struct usb_bulk_state in_pipe = {0};
    ep.bEndpointAddress = 0x85;
    for (unsigned i = 0; i < 2; ++i) {
        assert(submit_bulk(&h, &ep, unaligned, 20, 100, &in_pipe) == 20);
        assert(last_pid == (i & 1U)); clean();
    }
    assert(pipe.data_toggle == 1 && in_pipe.data_toggle == 0);
    ep.bEndpointAddress = 4;
    memset(bytes, 0x37, sizeof(bytes));
    /* A full packet and its terminating ZLP each consume one DATA PID. */
    static uint8_t full[512]; memset(full, 0x37, sizeof(full));
    assert(submit_bulk(&h, &ep, full, sizeof(full), 100, &pipe) == 512);
    assert(last_pid == 1 && pipe.data_toggle == 0); clean();
    assert(submit_bulk(&h, &ep, NULL, 0, 100, &pipe) == 0);
    assert(last_pid == 0 && pipe.data_toggle == 1); clean();
    /* Failed submissions and timeouts without progress must not reset PID. */
    submit_error = -USB_ERR_IO;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, &pipe) == RTCP_EIO);
    assert(pipe.data_toggle == 1); clean(); submit_error = 0;
    timeout = 1; kill_error = 0;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, &pipe) == RTCP_ETIMEDOUT);
    assert(pipe.data_toggle == 1); clean(); timeout = 0;
    /* Late completion of an old generation cannot overwrite reset state. */
    disconnect_in_submit = 1;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, &pipe) == RTCP_ESTATE);
    assert(pipe.data_toggle == 0); clean(); disconnect_in_submit = 0;
    s_usb.mux_out_state.data_toggle = s_usb.mux_in_state.data_toggle = 1;
    uint32_t previous = s_usb.mux_out_state.generation;
    reset_bulk_states();
    assert(!s_usb.mux_out_state.data_toggle && !s_usb.mux_in_state.data_toggle);
    assert(s_usb.mux_out_state.generation == previous + 1);
    /* Completion during a diagnostic dump must not force failed cancel
     * quarantine for an already completed URB. */
    simulate_pid = 0; timeout = 1; diagnostic_completes = 1;
    int previous_kills = kills;
    ep.bEndpointAddress = 0x85;
    pipe.data_toggle = 0;
    /* A positive completion in this race must return data, preserve the
     * next DATA PID and leave a canary intact, rather than discard a TLS
     * fragment and report an idle timeout. */
    diagnostic_length = 7;
    memset(bytes, 0x37, sizeof(bytes));
    assert(submit_bulk(&h, &ep, unaligned, 9, 100, &pipe) == 7);
    assert(unaligned[0] == 0xa6 && unaligned[6] == 0xa6 && unaligned[7] == 0x37);
    assert(pipe.data_toggle == 1 && kills == previous_kills); clean();
    diagnostic_length = 0;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100, &pipe) == 0);
    assert(pipe.data_toggle == 0 && kills == previous_kills); clean();
    diagnostic_length = 7;
    diagnostic_error = -USB_ERR_IO;
    assert(submit_bulk(&h, &ep, unaligned, 9, 100, &pipe) == RTCP_EIO);
    assert(pipe.data_toggle == 0 && kills == previous_kills); clean();
    diagnostic_error = 0; diagnostic_disconnect = 1;
    assert(submit_bulk(&h, &ep, unaligned, 9, 100, &pipe) == RTCP_ESTATE);
    assert(pipe.data_toggle == 0 && kills == previous_kills); clean();
    diagnostic_disconnect = 0; diagnostic_length = 0;
    diagnostic_completes = 0;
    kill_completes = 1;
    memset(bytes, 0x37, sizeof(bytes));
    assert(submit_bulk(&h, &ep, unaligned, 9, 100, &pipe) == 3);
    assert(unaligned[0] == 0xa7 && unaligned[2] == 0xa7 && unaligned[3] == 0x37);
    assert(pipe.data_toggle == 1); clean();
    kill_completes = 0; timeout = 0;
    /* Public USBMUX IN operation follows usbmuxd's 16 KiB USB_MRU. */
    s_usb.present = true; s_usb.hport = &h; s_usb.mux_in = 0x85;
    s_usb.mux_in_desc = ep; s_usb.mux_in_desc.bEndpointAddress = 0x85;
    static uint8_t large[32768]; memset(large, 0x37, sizeof(large));
    simulate_pid = 0;
    assert(lubanlite_bulk_transfer(NULL, 0x85, large, sizeof(large), 100) == 16384);
    assert(large[16383] == 0xa5 && large[16384] == 0x37); clean();
    puts("Luban-Lite DMA alignment, copy bounds and cancellation tests passed");
}
