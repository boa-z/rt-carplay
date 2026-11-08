#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct iap2_core iap2_core_t;

typedef enum {
    IAP2_CORE_IDLE = 0,
    IAP2_CORE_WAIT_MARKER,
    IAP2_CORE_NEGOTIATING,
    IAP2_CORE_READY,
    IAP2_CORE_DEAD,
} iap2_core_state_t;

typedef bool (*iap2_core_tx_cb_t)(const uint8_t *bytes, size_t len, void *context);
typedef void (*iap2_core_message_cb_t)(uint16_t message_id,
                                       const uint8_t *body,
                                       size_t body_len,
                                       void *context);
typedef void (*iap2_core_state_cb_t)(iap2_core_state_t state,
                                     const char *reason,
                                     void *context);
typedef void (*iap2_core_csm_acked_cb_t)(uint16_t message_id, void *context);

iap2_core_t *iap2_core_create(iap2_core_tx_cb_t tx_cb,
                              iap2_core_message_cb_t message_cb,
                              iap2_core_state_cb_t state_cb,
                              void *context);
void iap2_core_destroy(iap2_core_t *core);
void iap2_core_reset(iap2_core_t *core, const char *reason);
bool iap2_core_start(iap2_core_t *core, uint64_t now_ms);
void iap2_core_feed(iap2_core_t *core, const uint8_t *bytes, size_t len, uint64_t now_ms);
void iap2_core_advance_time(iap2_core_t *core, uint64_t now_ms);
bool iap2_core_send_csm(iap2_core_t *core,
                        uint16_t message_id,
                        const uint8_t *body,
                        size_t body_len,
                        uint64_t now_ms);
/* Completion runs when the peer link acknowledges the CSM frame. */
bool iap2_core_send_csm_with_ack_callback(iap2_core_t *core,
                                          uint16_t message_id,
                                          const uint8_t *body,
                                          size_t body_len,
                                          iap2_core_csm_acked_cb_t acked_callback,
                                          void *acked_context,
                                          uint64_t now_ms);
iap2_core_state_t iap2_core_state(const iap2_core_t *core);
uint32_t iap2_core_rx_frames(const iap2_core_t *core);
uint32_t iap2_core_bad_frames(const iap2_core_t *core);

