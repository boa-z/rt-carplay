/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
/* Compile the actual SDK port, including its private DMA submission helpers. */
#include "../../port/luban-lite/rtcp_usb_lubanlite.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int allocations, semaphores, locked, fail_alloc, fail_sem;
static int submit_error, timeout, kill_error, short_by, extra, kills;
static struct usbh_urb *pending;
static struct usb_dma_transfer *allocation;
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
    assert(submit_bulk(&h, &ep, unaligned, 65, 100) == 62);
    assert(unaligned[61] == 0xa5 && unaligned[62] == 0x37); clean();
    short_by = 0; ep.bEndpointAddress = 4;
    memset(bytes, 0x37, sizeof(bytes));
    assert(submit_bulk(&h, &ep, unaligned, 129, 100) == 129); clean();
    assert(submit_bulk(&h, &ep, NULL, 0, 100) == 0); clean();
    extra = 1;
    assert(control_transfer_timeout(&h, 0x80, 6, 0, 0, unaligned, 9, 100) == RTCP_EIO); clean();
    extra = 0; short_by = 10;
    assert(control_transfer_timeout(&h, 0x80, 6, 0, 0, unaligned, 9, 100) == RTCP_EIO); clean();
    short_by = 0;
    assert(submit_bulk(&h, &ep, NULL, 1, 100) == RTCP_EINVAL); clean();
    assert(submit_bulk(&h, &ep, unaligned, SIZE_MAX, 100) == RTCP_EINVAL); clean();
    fail_alloc = 1;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100) == RTCP_ENOMEM); clean();
    fail_alloc = 0; fail_sem = 1;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100) == RTCP_ENOMEM); clean();
    fail_sem = 0; submit_error = -USB_ERR_STALL;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100) == RTCP_EIO); clean();
    submit_error = 0; timeout = 1;
    assert(submit_bulk(&h, &ep, unaligned, 1, 100) == RTCP_ETIMEDOUT); clean();
    assert(kills == 1); kill_error = -USB_ERR_TIMEOUT;
    assert(control_transfer_timeout(&h, 0x80, 6, 0, 0, unaligned, 9, 100) == RTCP_ETIMEDOUT);
    assert(allocations == 1 && semaphores == 1 && !locked);
    /* A late callback and DMA write remain valid after the caller returns. */
    pending->transfer_buffer[0] = 0xff;
    pending->complete(pending->arg, 1);
    usb_osal_sem_delete(allocation->done); rt_free_align(allocation); clean();
    puts("Luban-Lite DMA alignment, copy bounds and cancellation tests passed");
}
