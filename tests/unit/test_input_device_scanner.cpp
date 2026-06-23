// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_device_scanner.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "../catch_amalgamated.hpp"

using helix::input::check_capability_bit;

namespace fs = std::filesystem;

namespace {

struct MockInputTree {
    std::string base;
    std::string dev_dir;
    std::string sysfs_dir;

    explicit MockInputTree(const std::string& label) {
        base = "/tmp/helix_test_input_" + label + "_" +
               std::to_string(static_cast<unsigned long>(time(nullptr)));
        dev_dir = base + "/dev/input";
        sysfs_dir = base + "/sys/class/input";
        fs::create_directories(dev_dir);
        fs::create_directories(sysfs_dir);
    }

    ~MockInputTree() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    // bustype: "0003"=USB, "0005"=Bluetooth, "0019"=host/platform, ""=omit
    void add_device(int event_num, const std::string& name,
                    const std::map<std::string, std::string>& caps,
                    const std::string& bustype = "0003") {
        std::string dev_path = dev_dir + "/event" + std::to_string(event_num);
        std::ofstream(dev_path).put('x');

        std::string sysfs_path = sysfs_dir + "/event" + std::to_string(event_num);
        fs::create_directories(sysfs_path + "/device/capabilities");
        fs::create_directories(sysfs_path + "/device/id");

        std::ofstream(sysfs_path + "/device/name") << name;

        if (!bustype.empty()) {
            std::ofstream(sysfs_path + "/device/id/bustype") << bustype;
        }

        for (const auto& [cap_name, hex_value] : caps) {
            std::ofstream(sysfs_path + "/device/capabilities/" + cap_name) << hex_value;
        }

        // Write vendor/product ID files if the bustype is USB or Bluetooth
        if (bustype == "0003" || bustype == "0005") {
            std::ofstream(sysfs_path + "/device/id/vendor") << "1a2c";
            std::ofstream(sysfs_path + "/device/id/product")
                << std::string("000") + std::to_string(event_num);
        }
    }

    void add_device_with_ids(int event_num, const std::string& name,
                             const std::map<std::string, std::string>& caps,
                             const std::string& bustype, const std::string& vendor,
                             const std::string& product) {
        add_device(event_num, name, caps, bustype);
        std::string sysfs_path = sysfs_dir + "/event" + std::to_string(event_num);
        // Overwrite the default IDs written by add_device
        std::ofstream(sysfs_path + "/device/id/vendor") << vendor;
        std::ofstream(sysfs_path + "/device/id/product") << product;
    }
};

} // namespace

TEST_CASE("check_capability_bit parses sysfs hex bitmasks", "[input]") {
    SECTION("empty string returns false") {
        REQUIRE_FALSE(check_capability_bit("", 0));
        REQUIRE_FALSE(check_capability_bit("", 30));
    }

    SECTION("bit 0 in single word") {
        REQUIRE(check_capability_bit("1", 0));
        REQUIRE_FALSE(check_capability_bit("0", 0));
    }

    SECTION("bit 1 in single word") {
        REQUIRE(check_capability_bit("3", 1));
        REQUIRE(check_capability_bit("2", 1));
        REQUIRE_FALSE(check_capability_bit("1", 1));
    }

    SECTION("KEY_A (bit 30) in last word") {
        REQUIRE(check_capability_bit("40000000", 30));
        REQUIRE_FALSE(check_capability_bit("20000000", 30));
    }

    SECTION("KEY_A in multi-word bitmask (32-bit words)") {
        REQUIRE(check_capability_bit("10000 40000000", 30));
    }

    SECTION("KEY_A in multi-word bitmask (64-bit words)") {
        REQUIRE(check_capability_bit("10000 0000000040000000", 30));
    }

    SECTION("BTN_LEFT (bit 272) in 32-bit word format") {
        REQUIRE(check_capability_bit("10000 0 0 0 0 0 0 0 0", 272));
    }

    SECTION("BTN_LEFT (bit 272) in 64-bit word format") {
        // At least one word must have >8 hex digits to signal 64-bit
        REQUIRE(check_capability_bit("10000 0000000000000000 0 0 0", 272));
    }

    SECTION("BTN_LEFT not set") {
        REQUIRE_FALSE(check_capability_bit("0 0 0 0 0 0 0 0 0", 272));
    }

    SECTION("KEY_POWER (bit 116) — should not match KEY_A check") {
        REQUIRE(check_capability_bit("0 0 0 0 0 100000 0 0 0", 116));
        REQUIRE_FALSE(check_capability_bit("0 0 0 0 0 100000 0 0 0", 30));
    }

    SECTION("real-world keyboard capability string from Pi 5 (aarch64)") {
        // This keyboard has both KEY_A and BTN_LEFT set (combo keyboard+trackpad)
        const char* real_kb = "3 0 0 0 0 0 403ffff 73ffff206efffd f3cfffff ffffffff fffffffe";
        REQUIRE(check_capability_bit(real_kb, 30));  // KEY_A
        REQUIRE(check_capability_bit(real_kb, 272)); // BTN_LEFT (combo device)
    }

    SECTION("real-world mouse capability string") {
        const char* real_mouse_32 = "1f0000 0 0 0 0 0 0 0 0";
        REQUIRE(check_capability_bit(real_mouse_32, 272));
        REQUIRE(check_capability_bit(real_mouse_32, 273));
        REQUIRE_FALSE(check_capability_bit(real_mouse_32, 30));

        const char* real_mouse_64 = "1f0000 0000000000000000 0 0 0";
        REQUIRE(check_capability_bit(real_mouse_64, 272));
    }

    SECTION("negative bit number returns false") {
        REQUIRE_FALSE(check_capability_bit("ffffffff", -1));
    }
}

TEST_CASE("find_mouse_device detects USB HID mice via sysfs", "[input]") {
    using helix::input::find_mouse_device;

    SECTION("detects mouse with REL_X + REL_Y + BTN_LEFT") {
        MockInputTree tree("mouse_basic");
        tree.add_device(3, "Logitech USB Mouse",
                        {{"rel", "3"}, {"key", "1f0000 0 0 0 0 0 0 0 0"}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Logitech USB Mouse");
        REQUIRE(result->event_num == 3);
    }

    SECTION("skips touchscreen with ABS_X + ABS_Y") {
        MockInputTree tree("mouse_skip_touch");
        tree.add_device(0, "Goodix Touchscreen", {{"abs", "3"}, {"rel", "0"}, {"key", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device without BTN_LEFT") {
        MockInputTree tree("mouse_no_btn");
        tree.add_device(1, "Some Sensor", {{"rel", "3"}, {"key", "0"}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("empty directory returns nullopt") {
        MockInputTree tree("mouse_empty");
        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("picks mouse when touchscreen also present") {
        MockInputTree tree("mouse_with_touch");
        tree.add_device(0, "Goodix Touchscreen", {{"abs", "3"}, {"rel", "0"}, {"key", "0"}});
        tree.add_device(2, "USB Mouse",
                        {{"rel", "3"}, {"key", "1f0000 0 0 0 0 0 0 0 0"}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Mouse");
        REQUIRE(result->event_num == 2);
    }

    SECTION("skips device with both ABS and REL (touchscreen with mouse emulation)") {
        MockInputTree tree("mouse_abs_rel");
        tree.add_device(0, "TouchMouseCombo",
                        {{"abs", "3"}, {"rel", "3"}, {"key", "1f0000 0 0 0 0 0 0 0 0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips MT-only touchscreen (ABS_MT_POSITION_X/Y without legacy ABS_X/Y)") {
        MockInputTree tree("mouse_mt_only");
        // Goodix GT911 MT-only: ABS_MT_POSITION_X=53, ABS_MT_POSITION_Y=54
        // bits 53+54 set = 0x60000000000000 in a 64-bit word, or
        // in 32-bit: bit 53 = word 1 bit 21, bit 54 = word 1 bit 22
        // = 0x600000 in word 1 from right
        tree.add_device(0, "Goodix Capacitive TouchScreen",
                        {
                            {"abs", "600000 0"}, // ABS_MT_POSITION_X(53) + ABS_MT_POSITION_Y(54)
                            {"rel", "0"},
                            {"key", "400 0 0 0 0 0 0 0 0 0 0"} // BTN_TOUCH(330)
                        });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device with BTN_TOUCH even without ABS axes") {
        MockInputTree tree("mouse_btn_touch");
        // Hypothetical device with REL_X/REL_Y + BTN_LEFT but also BTN_TOUCH
        tree.add_device(
            0, "WeirdTouchDevice",
            {
                {"abs", "0"},
                {"rel", "3"},
                {"key", "400 0 1f0000 0 0 0 0 0 0 0 0"} // BTN_TOUCH(330) + BTN_LEFT(272)
            });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("still detects real mouse when MT-only touchscreen present") {
        MockInputTree tree("mouse_with_mt_touch");
        tree.add_device(0, "Goodix Capacitive TouchScreen",
                        {{"abs", "600000 0"}, {"rel", "0"}, {"key", "400 0 0 0 0 0 0 0 0 0 0"}});
        tree.add_device(2, "USB Mouse",
                        {{"rel", "3"}, {"key", "1f0000 0 0 0 0 0 0 0 0"}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Mouse");
    }

    SECTION("skips MCE IR receiver with mouse capabilities (non-USB bus)") {
        MockInputTree tree("mouse_mce_ir");
        // Real-world: Allwinner sunxi-ir-tx MCE device has REL_X+Y, BTN_LEFT,
        // full keyboard keys, but is on the platform bus (0x0019), not USB.
        tree.add_device(
            3, "MCE IR Keyboard/Mouse (sunxi-ir-tx)",
            {{"abs", "0"},
             {"rel", "3"},
             {"key", "30000 0 7 ff87207a c14057ff febeffdf ffefffff ffffffff fffffffe"}},
            "0019"); // BUS_HOST

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device with no bustype file") {
        MockInputTree tree("mouse_no_bus");
        tree.add_device(0, "Virtual Mouse",
                        {{"rel", "3"}, {"key", "1f0000 0 0 0 0 0 0 0 0"}, {"abs", "0"}},
                        ""); // No bustype file

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("detects Bluetooth mouse") {
        MockInputTree tree("mouse_bt");
        tree.add_device(0, "BT Mouse",
                        {{"rel", "3"}, {"key", "1f0000 0 0 0 0 0 0 0 0"}, {"abs", "0"}},
                        "0005"); // BUS_BLUETOOTH

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "BT Mouse");
    }
}

TEST_CASE("find_keyboard_device detects USB HID keyboards via sysfs", "[input]") {
    using helix::input::find_keyboard_device;

    SECTION("detects keyboard with KEY_A") {
        MockInputTree tree("kb_basic");
        tree.add_device(1, "USB Keyboard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("skips power button (KEY_POWER=116 but no KEY_A)") {
        MockInputTree tree("kb_power");
        tree.add_device(0, "Power Button",
                        {{"key", "0 0 0 0 0 100000 0 0 0"}, {"rel", "0"}, {"abs", "0"}}, "0019");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("finds keyboard among other devices") {
        MockInputTree tree("kb_mixed");
        tree.add_device(0, "Goodix Touchscreen", {{"key", "0"}, {"abs", "3"}, {"rel", "0"}},
                        "0018");
        tree.add_device(1, "Power Button", {{"key", "100000"}, {"abs", "0"}, {"rel", "0"}}, "0019");
        tree.add_device(2, "USB Keyboard", {{"key", "10000 40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("empty directory returns nullopt") {
        MockInputTree tree("kb_empty");
        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("combo keyboard+mouse device detected as keyboard") {
        MockInputTree tree("kb_combo");
        tree.add_device(0, "Logitech K400",
                        {{"key", "1f0000 0 0 0 0 0 0 0 40000000"}, {"rel", "3"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Logitech K400");
    }

    SECTION("skips MCE IR device with KEY_A (non-USB bus)") {
        MockInputTree tree("kb_mce_ir");
        tree.add_device(
            3, "MCE IR Keyboard/Mouse (sunxi-ir-tx)",
            {{"abs", "0"},
             {"rel", "3"},
             {"key", "30000 0 7 ff87207a c14057ff febeffdf ffefffff ffffffff fffffffe"}},
            "0019");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("find_keyboard_device excludes barcode scanners", "[input]") {
    using helix::input::find_keyboard_device;

    SECTION("skips device with 'barcode' in name") {
        MockInputTree tree("kb_excl_barcode");
        tree.add_device(1, "Tera Barcode Scanner",
                        {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device with 'scanner' in name (case-insensitive)") {
        MockInputTree tree("kb_excl_scanner");
        tree.add_device(1, "QR SCANNER Pro", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device matching exclude_vendor_product") {
        MockInputTree tree("kb_excl_vid");
        tree.add_device_with_ids(1, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("returns real keyboard when scanner is excluded by vendor:product") {
        MockInputTree tree("kb_excl_with_real");
        tree.add_device_with_ids(1, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "1a2c",
                                 "4c5e");
        tree.add_device(2, "USB Keyboard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("returns real keyboard when scanner is excluded by name") {
        MockInputTree tree("kb_excl_name_real");
        tree.add_device(1, "Tera Barcode Scanner",
                        {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});
        tree.add_device(2, "USB Keyboard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("no exclude param still returns generic HID keyboard") {
        MockInputTree tree("kb_excl_none");
        tree.add_device(1, "TMS HIDKeyBoard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "TMS HIDKeyBoard");
    }
}

TEST_CASE("find_hid_keyboard_devices detects USB HID keyboards for scanner use", "[input]") {
    using helix::input::find_hid_keyboard_devices;

    SECTION("detects generic USB HID keyboard (e.g. QR scanner reporting as USBKey)") {
        MockInputTree tree("hid_generic");
        tree.add_device(0, "goodix-ts", {{"abs", "3"}, {"key", "0"}, {"rel", "0"}}, "0018");
        tree.add_device(2, "USBKey Chip USBKey Module",
                        {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "USBKey Chip USBKey Module");
    }

    SECTION("named barcode scanner is returned before generic keyboard") {
        MockInputTree tree("hid_priority");
        tree.add_device(1, "USBKey Chip USBKey Module",
                        {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});
        tree.add_device(2, "Tera Barcode Scanner",
                        {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 2);
        // Named scanner should come first
        REQUIRE(result[0].name == "Tera Barcode Scanner");
        REQUIRE(result[1].name == "USBKey Chip USBKey Module");
    }

    SECTION("skips touchscreen devices") {
        MockInputTree tree("hid_skip_touch");
        tree.add_device(0, "goodix-ts", {{"abs", "3"}, {"key", "40000000"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("skips non-USB devices") {
        MockInputTree tree("hid_skip_platform");
        tree.add_device(0, "MCE IR Keyboard", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}},
                        "0019");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("skips devices without KEY_A") {
        MockInputTree tree("hid_skip_no_keya");
        tree.add_device(0, "Power Button", {{"key", "100000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("returns multiple USB HID devices") {
        MockInputTree tree("hid_multi");
        tree.add_device(1, "USB Keyboard", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});
        tree.add_device(3, "QR Scanner", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 2);
    }

    SECTION("empty directory returns empty vector") {
        MockInputTree tree("hid_empty");
        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("configured vendor:product device wins over named scanner") {
        MockInputTree tree("hid_configured");
        tree.add_device_with_ids(1, "Tera Barcode Scanner",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "aaaa",
                                 "bbbb");
        tree.add_device_with_ids(2, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "TMS HIDKeyBoard");
    }

    SECTION("configured device not found falls back to normal priority") {
        MockInputTree tree("hid_configured_missing");
        tree.add_device_with_ids(1, "Tera Barcode Scanner",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "aaaa",
                                 "bbbb");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "Tera Barcode Scanner");
    }

    SECTION("empty configured string uses normal priority") {
        MockInputTree tree("hid_configured_empty");
        tree.add_device_with_ids(1, "USBKey Module",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1234",
                                 "5678");
        tree.add_device_with_ids(2, "Tera Barcode Scanner",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "aaaa",
                                 "bbbb");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, "");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0].name == "Tera Barcode Scanner");
    }
}

TEST_CASE("enumerate_usb_hid_devices returns devices with vendor/product IDs", "[input]") {
    using helix::input::enumerate_usb_hid_devices;

    SECTION("returns device with correct vendor/product IDs") {
        MockInputTree tree("enum_basic");
        tree.add_device_with_ids(2, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "TMS HIDKeyBoard");
        REQUIRE(result[0].vendor_id == "1a2c");
        REQUIRE(result[0].product_id == "4c5e");
        REQUIRE(result[0].event_path.find("event2") != std::string::npos);
    }

    SECTION("returns multiple devices") {
        MockInputTree tree("enum_multi");
        tree.add_device_with_ids(1, "USB Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "04d9",
                                 "a070");
        tree.add_device_with_ids(3, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 2);
    }

    SECTION("skips non-USB devices") {
        MockInputTree tree("enum_skip_platform");
        tree.add_device(0, "MCE IR Keyboard", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}},
                        "0019");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("skips touchscreens") {
        MockInputTree tree("enum_skip_touch");
        tree.add_device_with_ids(0, "Goodix TS", {{"abs", "3"}, {"key", "40000000"}, {"rel", "0"}},
                                 "0003", "1234", "5678");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("empty directory returns empty vector") {
        MockInputTree tree("enum_empty");
        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }
}
