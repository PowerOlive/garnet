// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_VMM_DEVICE_DEVICE_BASE_H_
#define GARNET_BIN_GUEST_VMM_DEVICE_DEVICE_BASE_H_

#include <fuchsia/guest/device/cpp/fidl.h>
#include <lib/async/cpp/trap.h>
#include <lib/async/default.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding_set.h>
#include <zx/event.h>

#include "garnet/lib/machina/device/config.h"
#include "garnet/lib/machina/device/phys_mem.h"

template <typename T>
class DeviceBase {
 private:
  // Converts a device trap into queue notifications.
  void OnQueueNotify(async_dispatcher_t* dispatcher,
                     async::GuestBellTrapBase* trap, zx_status_t status,
                     const zx_packet_guest_bell_t* bell) {
    FXL_CHECK(status == ZX_OK) << "Device trap failed " << status;
    uint16_t queue = machina::queue_from(trap_addr_, bell->addr);
    static_cast<T*>(this)->NotifyQueue(queue);
  }

 protected:
  fidl::BindingSet<T> bindings_;
  zx_gpaddr_t trap_addr_;
  zx::event event_;
  machina::PhysMem phys_mem_;
  async::GuestBellTrapMethod<DeviceBase, &DeviceBase::OnQueueNotify> trap_{
      this};

  DeviceBase(component::StartupContext* context) {
    context->outgoing().AddPublicService(
        bindings_.GetHandler(static_cast<T*>(this)));
  }

  // Prepares a device to start.
  void PrepStart(fuchsia::guest::device::StartInfo start_info) {
    FXL_CHECK(!event_) << "Device has already been started";

    event_ = std::move(start_info.event);
    zx_status_t status = phys_mem_.Init(std::move(start_info.vmo));
    FXL_CHECK(status == ZX_OK)
        << "Failed to init guest physical memory " << status;

    if (start_info.guest) {
      trap_addr_ = start_info.trap.addr;
      status = trap_.SetTrap(async_get_default_dispatcher(), start_info.guest,
                             start_info.trap.addr, start_info.trap.size);
      FXL_CHECK(status == ZX_OK) << "Failed to set trap " << status;
    }
  }

  // Signals an interrupt for the device.
  zx_status_t Interrupt(uint8_t actions) {
    return event_.signal(0, static_cast<zx_signals_t>(actions)
                                << machina::kDeviceInterruptShift);
  }
};

#endif  // GARNET_BIN_GUEST_VMM_DEVICE_DEVICE_BASE_H_
