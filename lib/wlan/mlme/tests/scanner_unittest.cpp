// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <wlan/mlme/client/scanner.h>

#include "mock_device.h"

#include <lib/timekeeper/clock.h>
#include <wlan/mlme/client/channel_scheduler.h>
#include <wlan/mlme/device_interface.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/mlme.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/service.h>
#include <wlan/mlme/timer.h>
#include <wlan/protocol/mac.h>

#include <fbl/ref_ptr.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/wlan/mlme/c/fidl.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <gtest/gtest.h>
#include <zircon/status.h>

#include <cstring>

namespace wlan {

namespace wlan_mlme = ::fuchsia::wlan::mlme;

namespace {

const uint8_t kBeacon[] = {
    0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x00, 0x01, 0x00, 0x00, 0x09, 0x74, 0x65, 0x73, 0x74, 0x20, 0x73, 0x73, 0x69, 0x64,
};

template <typename T>
static fbl::unique_ptr<Packet> IntoPacket(const T& msg, uint32_t ordinal = 42) {
    // fidl2 doesn't have a way to get the serialized size yet. 4096 bytes should be enough for
    // everyone.
    const size_t kBufLen = 4096;
    auto packet = GetSvcPacket(kBufLen);
    memset(packet->mut_data(), 0, kBufLen);
    SerializeServiceMsg(packet.get(), ordinal, msg.get());
    return fbl::move(packet);
}

struct MockOnChannelHandler : OnChannelHandler {
    virtual void HandleOnChannelFrame(fbl::unique_ptr<Packet>) override {}
    virtual void PreSwitchOffChannel() override {}
    virtual void ReturnedOnChannel() override {}
};

class ScannerTest : public ::testing::Test {
   public:
    ScannerTest()
        : chan_sched_(&on_channel_handler_, &mock_dev_, mock_dev_.CreateTimer(1u)),
          scanner_(&mock_dev_, &chan_sched_) {
        mock_dev_.SetChannel(wlan_channel_t{.primary = 11, .cbw = CBW20});
        SetupMessages();
    }

   protected:
    void SetupMessages() {
        req_ = wlan_mlme::ScanRequest::New();
        req_->txn_id = 123;
        req_->channel_list.resize(0);
        req_->channel_list->push_back(1);
        req_->max_channel_time = 1u;
        req_->ssid.resize(0);
    }

    zx_status_t Start() {
        auto pkt = IntoPacket(req_, fuchsia_wlan_mlme_MLMEStartScanOrdinal);
        MlmeMsg<wlan_mlme::ScanRequest> start_req;
        if (MlmeMsg<wlan_mlme::ScanRequest>::FromPacket(fbl::move(pkt), &start_req) != ZX_OK) {
            return ZX_ERR_IO;
        }
        return scanner_.Start(start_req);
    }

    wlan_mlme::ScanResult ExpectScanResult() {
        wlan_mlme::ScanResult result;
        zx_status_t st =
            mock_dev_.GetQueuedServiceMsg(fuchsia_wlan_mlme_MLMEOnScanResultOrdinal, &result);
        EXPECT_EQ(ZX_OK, st);
        return result;
    }

    wlan_mlme::ScanEnd ExpectScanEnd() {
        wlan_mlme::ScanEnd scan_end;
        zx_status_t st =
            mock_dev_.GetQueuedServiceMsg(fuchsia_wlan_mlme_MLMEOnScanEndOrdinal, &scan_end);
        EXPECT_EQ(ZX_OK, st);
        EXPECT_EQ(123u, scan_end.txn_id);
        return scan_end;
    }

    wlan_mlme::ScanRequestPtr req_;
    MockDevice mock_dev_;
    MockOnChannelHandler on_channel_handler_;
    ChannelScheduler chan_sched_;
    Scanner scanner_;
};

TEST_F(ScannerTest, Start) {
    EXPECT_EQ(11u, mock_dev_.GetChannelNumber());
    EXPECT_FALSE(scanner_.IsRunning());

    EXPECT_EQ(ZX_OK, Start());
    EXPECT_TRUE(scanner_.IsRunning());

    EXPECT_EQ(1u, mock_dev_.GetChannelNumber());
}

TEST_F(ScannerTest, Start_InvalidChannelTimes) {
    req_->min_channel_time = 2;
    req_->max_channel_time = 1;

    EXPECT_EQ(11u, mock_dev_.GetChannelNumber());

    EXPECT_EQ(ZX_ERR_INVALID_ARGS, Start());
    EXPECT_FALSE(scanner_.IsRunning());
    EXPECT_EQ(11u, mock_dev_.GetChannelNumber());

    auto scan_end = ExpectScanEnd();
    EXPECT_EQ(wlan_mlme::ScanResultCodes::INVALID_ARGS, scan_end.code);
}

TEST_F(ScannerTest, Start_NoChannels) {
    SetupMessages();
    req_->channel_list.resize(0);

    EXPECT_EQ(11u, mock_dev_.GetChannelNumber());

    EXPECT_EQ(ZX_ERR_INVALID_ARGS, Start());
    EXPECT_FALSE(scanner_.IsRunning());
    EXPECT_EQ(11u, mock_dev_.GetChannelNumber());

    auto scan_end = ExpectScanEnd();
    EXPECT_EQ(wlan_mlme::ScanResultCodes::INVALID_ARGS, scan_end.code);
}

TEST_F(ScannerTest, Reset) {
    ASSERT_EQ(ZX_OK, Start());
    ASSERT_TRUE(scanner_.IsRunning());

    scanner_.Reset();
    EXPECT_FALSE(scanner_.IsRunning());
    // TODO(tkilbourn): check all the other invariants
}

TEST_F(ScannerTest, ScanChannel) {
    ASSERT_EQ(ZX_OK, Start());
    auto chan = scanner_.ScanChannel();
    EXPECT_EQ(1u, chan.primary);
}

TEST_F(ScannerTest, Timeout_NextChannel) {
    req_->min_channel_time = 1;
    req_->max_channel_time = 10;
    req_->channel_list.push_back(2);

    EXPECT_EQ(11u, mock_dev_.GetChannelNumber());

    ASSERT_EQ(ZX_OK, Start());
    ASSERT_EQ(1u, scanner_.ScanChannel().primary);

    EXPECT_EQ(1u, mock_dev_.GetChannelNumber());

    mock_dev_.AdvanceTime(WLAN_TU(req_->max_channel_time));
    chan_sched_.HandleTimeout();
    EXPECT_EQ(2u, scanner_.ScanChannel().primary);

    EXPECT_EQ(2u, mock_dev_.GetChannelNumber());
}

TEST_F(ScannerTest, ScanResponse) {
    ASSERT_EQ(ZX_OK, Start());

    wlan_rx_info_t info;
    info.valid_fields = WLAN_RX_INFO_VALID_RSSI | WLAN_RX_INFO_VALID_SNR;
    info.chan = {
        .primary = 1,
    };
    info.rssi_dbm = -75;
    info.snr_dbh = 30;

    auto buffer = GetBuffer(sizeof(kBeacon));
    auto packet = fbl::make_unique<Packet>(fbl::move(buffer), sizeof(kBeacon));
    packet->CopyCtrlFrom(info);
    memcpy(packet->mut_field<uint8_t*>(0), kBeacon, sizeof(kBeacon));

    chan_sched_.HandleIncomingFrame(fbl::move(packet));

    mock_dev_.SetTime(zx::time(1));
    chan_sched_.HandleTimeout();

    auto bss = ExpectScanResult().bss;
    EXPECT_EQ(0, std::memcmp(kBeacon + 16, bss.bssid.data(), 6));
    EXPECT_EQ(bss.ssid->size(), static_cast<size_t>(9));

    const uint8_t ssid[] = {'t', 'e', 's', 't', ' ', 's', 's', 'i', 'd'};
    EXPECT_EQ(0, std::memcmp(ssid, bss.ssid->data(), sizeof(ssid)));
    EXPECT_EQ(wlan_mlme::BSSTypes::INFRASTRUCTURE, bss.bss_type);
    EXPECT_EQ(100u, bss.beacon_period);
    EXPECT_EQ(1024u, bss.timestamp);
    // EXPECT_EQ(1u, bss->channel);  // IE missing. info.chan != bss->channel.
    EXPECT_EQ(-75, bss.rssi_dbm);
    EXPECT_EQ(WLAN_RCPI_DBMH_INVALID, bss.rcpi_dbmh);
    EXPECT_EQ(30, bss.rsni_dbh);

    auto scan_end = ExpectScanEnd();
    EXPECT_EQ(wlan_mlme::ScanResultCodes::SUCCESS, scan_end.code);
}

}  // namespace
}  // namespace wlan
