// SPDX-License-Identifier: Apache-2.0
//
// Detect macOS sleep/wake and wait for network mounts to recover.
// After wake, NFS/SMB mounts (e.g. /rdo/) go stale, causing the
// media pipeline and OCIO config to fail. We poll until the mount
// is back so downstream retries succeed.

#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <thread>
#include <cstdio>
#include "xstudio/utility/logging.hpp"

static io_connect_t   g_root_port = 0;
static IONotificationPortRef g_notify_port = NULL;
static io_object_t    g_notifier_object = 0;
static CFRunLoopRef   g_run_loop = NULL;

// Paths to check — first match wins. Covers both direct NFS/SMB
// mounts and Volumes-style mounts.
static const char *g_mount_paths[] = {
    "/rdo",
    "/Volumes/rdo",
    NULL
};

static void wait_for_mount_recovery() {
    std::thread([] {
        const int max_attempts = 120;       // 60 seconds at 500ms intervals
        const int interval_ms  = 500;

        // Find which mount path exists (was valid before sleep)
        const char *mount = NULL;
        for (int i = 0; g_mount_paths[i]; ++i) {
            struct stat st;
            // Use lstat — doesn't follow symlinks, doesn't hang on stale mounts
            if (lstat(g_mount_paths[i], &st) == 0) {
                mount = g_mount_paths[i];
                break;
            }
        }

        if (!mount) {
            fprintf(stderr, "[sleep_observer] no known mount path found, skipping wait\n");
            spdlog::warn("sleep_observer: no known mount path found, skipping wait");
            return;
        }

        fprintf(stderr, "[sleep_observer] waiting for mount '%s' to recover...\n", mount);
        spdlog::warn("sleep_observer: waiting for mount '{}' to recover", mount);

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            struct timespec ts = {0, interval_ms * 1000000L};
            nanosleep(&ts, NULL);

            // Try to stat a path UNDER the mount — the mount point itself
            // may return success even when the share is stale
            char test_path[512];
            snprintf(test_path, sizeof(test_path), "%s/.", mount);

            struct stat st;
            if (stat(test_path, &st) == 0) {
                fprintf(stderr,
                    "[sleep_observer] mount '%s' recovered after %.1fs\n",
                    mount, (attempt + 1) * interval_ms / 1000.0);
                spdlog::warn(
                    "sleep_observer: mount '{}' recovered after {:.1f}s",
                    mount, (attempt + 1) * interval_ms / 1000.0);
                return;
            }
        }

        fprintf(stderr,
            "[sleep_observer] mount '%s' did not recover within %ds\n",
            mount, max_attempts * interval_ms / 1000);
        spdlog::warn(
            "sleep_observer: mount '{}' did not recover within {}s",
            mount, max_attempts * interval_ms / 1000);
    }).detach();
}

static void power_callback(
    void */*refcon*/,
    io_service_t /*service*/,
    natural_t messageType,
    void *messageArgument)
{
    switch (messageType) {
        case kIOMessageCanSystemSleep:
            IOAllowPowerChange(g_root_port, (long)messageArgument);
            break;

        case kIOMessageSystemWillSleep:
            fprintf(stderr, "[sleep_observer] system will sleep\n");
            spdlog::warn("sleep_observer: system will sleep");
            IOAllowPowerChange(g_root_port, (long)messageArgument);
            break;

        case kIOMessageSystemHasPoweredOn:
            fprintf(stderr, "[sleep_observer] system powered on\n");
            spdlog::warn("sleep_observer: system powered on");
            wait_for_mount_recovery();
            break;
    }
}

void register_macos_sleep_observer() {
    if (g_root_port) return;

    std::thread([] {
        g_root_port = IORegisterForSystemPower(
            NULL, &g_notify_port, power_callback, &g_notifier_object);

        if (!g_root_port) {
            fprintf(stderr, "[sleep_observer] ERROR: IORegisterForSystemPower failed\n");
            spdlog::error("sleep_observer: IORegisterForSystemPower failed");
            return;
        }

        g_run_loop = CFRunLoopGetCurrent();
        CFRunLoopAddSource(
            g_run_loop,
            IONotificationPortGetRunLoopSource(g_notify_port),
            kCFRunLoopDefaultMode);

        fprintf(stderr, "[sleep_observer] registered on dedicated thread\n");
        spdlog::warn("sleep_observer: registered on dedicated thread");

        CFRunLoopRun();
    }).detach();
}

void unregister_macos_sleep_observer() {
    if (g_root_port) {
        if (g_run_loop) {
            CFRunLoopStop(g_run_loop);
            g_run_loop = NULL;
        }
        IODeregisterForSystemPower(&g_notifier_object);
        if (g_notify_port) {
            IONotificationPortDestroy(g_notify_port);
            g_notify_port = NULL;
        }
        g_root_port = 0;
        g_notifier_object = 0;
    }
}
