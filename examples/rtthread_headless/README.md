[English](README.md) | [简体中文](README.zh-CN.md)

# Standalone RT-Thread Headless receiver

## Scope

`headless.c` is an executable Receiver composition for an ordinary RT-Thread worker. It creates/loads a software Ed25519 identity, loads/saves the real SDK controller table, builds a video-only `/info` document, starts the public Receiver and optionally serves `_airplay._tcp.local` over mDNS. The same source runs with real Linux OS/network providers in the Host regression. It has no Luban-Lite, SoC, GUI, screen, sound or touch dependency. It is separate from the retired `examples/rtthread_app` board template and from the maintained D213 application.

This is a LAN protocol bench. Video is intentionally counted/discarded; audio is not advertised and no audio device is opened. It does not start USB enumeration, Bluetooth, MFi or Wi-Fi handoff, implement CarPlay Control reverse-connect policy, or claim automatic phone activation. Those services remain host responsibilities. Use the maintained Luban-Lite application for the complete D213 wired/wireless integration. This sample does not change that application's media negotiation or output behavior.

## Requirements and build

Use a C11 compiler, the portable SDK with software Ed25519 enabled, mbedTLS, an initialized RT-Thread BSD-socket network and a mounted standard-C filesystem. Build the SDK through its root `SConscript`; enable `LPKG_USING_RT_CARPLAY`, `LPKG_RT_CARPLAY_ED25519` and `LPKG_RT_CARPLAY_HEADLESS_EXAMPLE`. Include `examples/rtthread_headless` in the application's header path. The last option compiles the example but never autostarts a second Receiver. No deprecated board-template SConscript is used.

The generic `rtcp_os_rtthread.random` is deliberately NULL. Copy that OS table and supply a functioning cryptographically secure entropy callback from the actual platform. Missing entropy returns `RTCP_EINVAL`; provider failure returns `RTCP_EIO`, including when the identity file already exists. There is no weak RNG, simulated authentication, fixed private key or successful placeholder provider in this example. The MAC-format device ID shown below is a public example value; provision a stable unique ID per accessory.

## RT-Thread integration

Bring up the filesystem and network before calling the following wrapper from a serialized application control thread. Pass the actual platform CSPRNG to `start_headless()`. The wrapper starts the real blocking example on its sole owner worker; another thread requests stop and waits for completion. Its 32 KiB worker stack follows the existing receiver composition; measure actual board stack/memory use before product acceptance.

```c
#include <rtthread.h>
#include "rtcp_rtthread.h"
#include "headless.h"

static struct rtcp_os headless_os;
static rtcp_headless_config_t headless_config;
static rtcp_headless_result_t headless_result;
static atomic_bool stop_requested = ATOMIC_VAR_INIT(false);
static atomic_bool done = ATOMIC_VAR_INIT(true);
static int last_result;

static void headless_worker(void *argument)
{
    last_result = rtcp_headless_run(argument, &stop_requested, &headless_result);
    atomic_store_explicit(&done, true, memory_order_release);
}

int start_headless(bool (*secure_random)(uint8_t *, size_t))
{
    if (secure_random == NULL) return RTCP_EINVAL;
    bool expected = true;
    if (!atomic_compare_exchange_strong(&done, &expected, false)) return RTCP_ESTATE;
    headless_os = rtcp_os_rtthread;
    headless_os.random = secure_random;
    headless_config = (rtcp_headless_config_t){
        .os = &headless_os, .net = &rtcp_net_rtthread,
        .name = "Headless receiver", .device_id = "02:00:00:00:00:01",
        .seed_path = "/data/headless.seed", .store_path = "/data/headless.controllers",
        .width = 1024, .height = 600,
    };
    atomic_store(&stop_requested, false);
    rt_thread_t worker = rt_thread_create("headless", headless_worker,
                                         &headless_config, 32 * 1024, 15, 20);
    if (worker == NULL) { atomic_store(&done, true); return RTCP_ENOMEM; }
    if (rt_thread_startup(worker) != RT_EOK) {
        rt_thread_delete(worker);
        atomic_store(&done, true);
        return RTCP_EIO;
    }
    return RTCP_OK;
}

int stop_headless(void)
{
    atomic_store_explicit(&stop_requested, true, memory_order_release);
    for (unsigned waited = 0; !atomic_load_explicit(&done, memory_order_acquire) && waited < 3000;
         waited += 10) rt_thread_mdelay(10);
    return atomic_load_explicit(&done, memory_order_acquire) ? last_result : RTCP_ETIMEDOUT;
}
```

The wrapper functions are ordinary-thread operations and must be serialized by the application. Do not invoke `stop_headless()` from a Receiver callback or reset/free its configuration while a stop returns `RTCP_ETIMEDOUT`. Timeout means the worker still owns resources. A successful `start_headless()` means the worker was scheduled, not that protocol listeners are already ready; use `on_ready` or the eventual worker result to observe startup. Read `last_result` and `headless_result` only after acquiring a true `done` flag. Keep the OS/net tables, configuration strings and callback contexts alive until `rtcp_headless_run()` returns.

Set `advertise=true`, `host_name` and the real local `ipv4` to enable mDNS. Configure the outgoing multicast interface before starting if the network port needs that selection. `on_ready` receives the bound RTSP port and public key on the owner thread; copy the public key before returning if retaining it. `on_state` exposes Transport Connected, Authenticated, Session Established, Video Ready, Audio Ready and Input Ready independently through the public Receiver state; readiness is never inferred from a single connection bit. Callbacks must not block or start/stop the same Receiver recursively.

## Identity, persistence and ownership

Use a private mounted directory with appropriate access controls. The sample creates a 32-byte seed only when the seed file is absent, and never silently replaces corrupt or inaccessible identity data. Startup probes the actual entropy source even when reusing a seed. The controller store uses the SDK's whole-blob format and real save/lookup callbacks. Corrupt store data rejects startup. A failed pairing-store write propagates out of the running loop instead of being treated as durable success.

Writes use a same-directory `.tmp` file, move any existing primary to an absent `.bak`, then rename the temporary file into place. Failed commit attempts restore the previous primary; reads recover the backup only when the primary is absent. This supports FatFS rename without destination overwrite and both positive POSIX and negative DFS errno. A corrupt primary is still an error. The host must provide exclusive access to both paths; primary, temporary, backup paths and any symlink aliases must not collide. This example does not implement cross-process file locking, power-loss journaling, `fsync`, private-file permission provisioning or encrypted storage. Use a product storage backend when those guarantees are required. Never put seed files into a repository, artifact bundle or diagnostic log. Test temporary seeds are generated at runtime and deleted.

Only one example run can execute at a time. The SDK controller store is process-global, so the host must also exclude other compositions using that store. The worker owns create/start/poll/stop/destroy, identity, controller records and mDNS. Another thread may only set its atomic stop request, then join/wait. Cleanup closes discovery and destroys Receiver before retiring controller storage and clearing the software private key. No protocol callbacks follow return. Result counters and diagnostics are owner-written and may be read after joining, not as live concurrent snapshots.

## Verification and limits

`make -j8 check` includes `test_rtthread_headless`; `make -j8 build/test_rtthread_headless && build/test_rtthread_headless` runs it alone. The CMake test target has the same name:

```sh
cmake -S . -B build-cmake -DRTCP_BUILD_TESTS=ON -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
```

The actual example is linked with real Linux sockets, entropy, Ed25519 and controller-store implementations. Tests check TCP `/info` and its 1024×600 binary-plist geometry, a real UDP/mDNS query and parsed endpoint, stable identity and persisted controller records across three restarts, commit rollback and backup-only identity/controller recovery, nested-run exclusion, stop with a live client, pre-stopped startup, missing/failed entropy, inaccessible paths, corrupt seed/store, listener/discovery failures and persistence-write failure. The controller-store test path invokes the same configured SDK save/lookup callbacks directly on the owner; it does not claim a new full wire-level Pair-Setup test. Existing SDK encrypted Pair-Setup/Pair-Verify/media tests remain separate and unchanged. Every scenario runs with positive POSIX errno, negative DFS errno and FatFS no-overwrite rename semantics. ASan/UBSan covers this Host execution.

The maintained D213 Headless defconfig also enables compilation of this source against the real RT-Thread/BSP toolchain, without starting this additional bench. Host Tested and Cross-Compiled are separate evidence; neither establishes Hardware Validated. Physical startup, pairing, media reception, heap/stack use and stop/restart on a non-D213 RT-Thread board remain Pending Validation. This sample is not board acceptance for the D213 GUI or Launcher V2.
