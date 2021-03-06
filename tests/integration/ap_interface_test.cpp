/*
 * Copyright (C) 2016, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>

#include <gtest/gtest.h>
#include <utils/StrongPointer.h>
#include <wifi_system/interface_tool.h>

#include "android/net/wifi/IApInterface.h"
#include "android/net/wifi/IWificond.h"
#include "wificond/tests/integration/process_utils.h"

using android::net::wifi::IApInterface;
using android::net::wifi::IWificond;
using android::wifi_system::InterfaceTool;
using android::wificond::tests::integration::HostapdIsDead;
using android::wificond::tests::integration::HostapdIsRunning;
using android::wificond::tests::integration::ScopedDevModeWificond;
using android::wificond::tests::integration::WaitForTrue;
using std::string;
using std::vector;

namespace android {
namespace wificond {
namespace {

constexpr int kHostapdStartupTimeoutSeconds = 3;
constexpr int kHostapdDeathTimeoutSeconds = 3;

const char kValidSsid[] = "foobar";
const char kInvalidSsid[] = "0123456789"
                            "0123456789"
                            "0123456789"
                            "012";  // 33 bytes is too long
const char kValidPassphrase[] = "super secret";

}  // namespace

TEST(ApInterfaceTest, CanCreateApInterfaces) {
  ScopedDevModeWificond dev_mode;
  sp<IWificond> service = dev_mode.EnterDevModeOrDie();

  // We should be able to create an AP interface.
  sp<IApInterface> ap_interface;
  EXPECT_TRUE(service->createApInterface(&ap_interface).isOk());
  EXPECT_NE(nullptr, ap_interface.get());

  // The interface should start out down.
  string if_name;
  EXPECT_TRUE(ap_interface->getInterfaceName(&if_name).isOk());
  EXPECT_TRUE(!if_name.empty());
  InterfaceTool if_tool;
  EXPECT_FALSE(if_tool.GetUpState(if_name.c_str()));

  // Mark the interface as up, just to test that we mark it down on teardown.
  EXPECT_TRUE(if_tool.SetUpState(if_name.c_str(), true));
  EXPECT_TRUE(if_tool.GetUpState(if_name.c_str()));

  // We should not be able to create two AP interfaces.
  sp<IApInterface> ap_interface2;
  EXPECT_TRUE(service->createApInterface(&ap_interface2).isOk());
  EXPECT_EQ(nullptr, ap_interface2.get());

  // We can tear down the created interface.
  EXPECT_TRUE(service->tearDownInterfaces().isOk());
  EXPECT_FALSE(if_tool.GetUpState(if_name.c_str()));
}

// TODO: b/30311493 this test fails because hostapd fails to set the driver
//       channel every other time.
TEST(ApInterfaceTest, CanStartStopHostapd) {
  ScopedDevModeWificond dev_mode;
  sp<IWificond> service = dev_mode.EnterDevModeOrDie();
  sp<IApInterface> ap_interface;
  EXPECT_TRUE(service->createApInterface(&ap_interface).isOk());
  ASSERT_NE(nullptr, ap_interface.get());

  // Interface should start out down.
  string if_name;
  EXPECT_TRUE(ap_interface->getInterfaceName(&if_name).isOk());
  EXPECT_TRUE(!if_name.empty());
  InterfaceTool if_tool;
  EXPECT_FALSE(if_tool.GetUpState(if_name.c_str()));

  bool wrote_config = false;
  EXPECT_TRUE(ap_interface->writeHostapdConfig(
      vector<uint8_t>(kValidSsid, kValidSsid + sizeof(kValidSsid) - 1),
      false,
      6,
      IApInterface::ENCRYPTION_TYPE_WPA2,
      vector<uint8_t>(kValidPassphrase,
                      kValidPassphrase + sizeof(kValidPassphrase) - 1),
      &wrote_config).isOk());
  ASSERT_TRUE(wrote_config);

  for (int iteration = 0; iteration < 4; iteration++) {
    bool hostapd_started = false;
    EXPECT_TRUE(ap_interface->startHostapd(&hostapd_started).isOk());
    EXPECT_TRUE(hostapd_started);

    EXPECT_TRUE(WaitForTrue(HostapdIsRunning, kHostapdStartupTimeoutSeconds))
        << "Failed on iteration " << iteration;

    // There are two reasons to do this:
    //   1) We look for hostapd so quickly that we miss when it dies on startup
    //   2) If we don't give hostapd enough time to get fully up, killing it
    //      can leave the driver in a poor state.
    // The latter points to an obvious race, where we cannot fully clean up the
    // driver on quick transitions.
    auto InterfaceIsUp = [&if_tool, &if_name] () {
      return if_tool.GetUpState(if_name.c_str());
    };
    EXPECT_TRUE(WaitForTrue(InterfaceIsUp, kHostapdStartupTimeoutSeconds))
        << "Failed on iteration " << iteration;
    EXPECT_TRUE(HostapdIsRunning()) << "Failed on iteration " << iteration;

    bool hostapd_stopped = false;
    EXPECT_TRUE(ap_interface->stopHostapd(&hostapd_stopped).isOk());
    EXPECT_TRUE(hostapd_stopped);
    EXPECT_FALSE(if_tool.GetUpState(if_name.c_str()));


    EXPECT_TRUE(WaitForTrue(HostapdIsDead, kHostapdDeathTimeoutSeconds))
        << "Failed on iteration " << iteration;
  }
}

TEST(ApInterfaceTest, CanWriteHostapdConfig) {
  ScopedDevModeWificond dev_mode;
  sp<IWificond> service = dev_mode.EnterDevModeOrDie();
  sp<IApInterface> ap_interface;
  EXPECT_TRUE(service->createApInterface(&ap_interface).isOk());
  ASSERT_NE(nullptr, ap_interface.get());

  bool success = false;
  // Should be able to write out a valid configuration
  EXPECT_TRUE(ap_interface->writeHostapdConfig(
      vector<uint8_t>(kValidSsid, kValidSsid + sizeof(kValidSsid) - 1),
      false,
      2,
      IApInterface::ENCRYPTION_TYPE_WPA2,
      vector<uint8_t>(kValidPassphrase,
                      kValidPassphrase + sizeof(kValidPassphrase) - 1),
      &success).isOk());
  EXPECT_TRUE(success) << "Expected to write out a valid config.";

  // SSIDs have to be 32 bytes or less
  EXPECT_TRUE(ap_interface->writeHostapdConfig(
      vector<uint8_t>(kInvalidSsid, kInvalidSsid + sizeof(kInvalidSsid) - 1),
      false,
      2,
      IApInterface::ENCRYPTION_TYPE_WPA2,
      vector<uint8_t>(kValidPassphrase,
                      kValidPassphrase + sizeof(kValidPassphrase) - 1),
      &success).isOk());
  EXPECT_FALSE(success) << "Did not expect to write out an invalid config.";
}

}  // namespace wificond
}  // namespace android
