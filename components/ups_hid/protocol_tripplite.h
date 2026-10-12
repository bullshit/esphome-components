#pragma once

#include "ups_hid.h"
#include <string>

namespace esphome {
namespace ups_hid {

/**
 * Tripp Lite HID protocol for the Smart LCD family (USB vendor 0x09AE).
 *
 * Report IDs come from a NUT usbhid-ups -DD dump of a SMART1500LCD
 * (09AE:2012), which shares NUT's battery_scale 0.1 bucket with 09AE:2009
 * (this cluster's SMART1500LCD). Confirmed on the live 09AE:2009 unit:
 * input report 0x30 is 2 bytes (nominal voltage) and input report 0x31 is
 * 3 bytes of line voltage in tenths. The generic parser publishes 0x31 as
 * output voltage and never reads the reports below.
 *
 * Feature reports (input report of the same id is accepted as a fallback):
 *   0x34  RemainingCapacity     uint8 percent
 *   0x35  RunTimeToEmpty        uint16 seconds
 *   0x18  Input.Voltage         uint16 tenths of a volt
 *   0x1B  Output.Voltage        uint16 tenths of a volt
 *   0x1E  PercentLoad           uint8 percent
 *   0x32  PresentStatus         bit1 AC, bit2 charging, bit3 discharging,
 *                               bit4 replace, bit5 low, bit6 full
 *   0x22  Converter status      bit4 overload
 *   0x31  PresentStatus.Voltage uint16 tenths; used only if 0x18 is absent
 */
class TrippLiteProtocol : public UpsProtocolBase {
 public:
  TrippLiteProtocol(UpsHidComponent *parent) : UpsProtocolBase(parent) {}

  bool detect() override;
  bool initialize() override;
  bool read_data(UpsData &data) override;
  DeviceInfo::DetectedProtocol get_protocol_type() const override { return DeviceInfo::PROTOCOL_TRIPPLITE_HID; }
  std::string get_protocol_name() const override { return "TrippLite HID"; }

 private:
  static const uint8_t REPORT_BATTERY_LEVEL = 0x34;
  static const uint8_t REPORT_RUNTIME = 0x35;
  static const uint8_t REPORT_INPUT_VOLTAGE = 0x18;
  static const uint8_t REPORT_OUTPUT_VOLTAGE = 0x1B;
  static const uint8_t REPORT_LOAD = 0x1E;
  static const uint8_t REPORT_PRESENT_STATUS = 0x32;
  static const uint8_t REPORT_CONVERTER_STATUS = 0x22;
  static const uint8_t REPORT_LINE_VOLTAGE = 0x31;
  static const uint8_t REPORT_INPUT_NOMINAL = 0x30;
  static const uint8_t REPORT_FREQUENCY = 0x19;
  static const uint8_t REPORT_BATTERY_VOLTAGE = 0x20;
  static const uint8_t REPORT_BATTERY_VOLTAGE_NOMINAL = 0x04;
  static const uint8_t REPORT_APPARENT_POWER = 0x03;

  static const uint8_t STATUS_AC_PRESENT = 1 << 1;
  static const uint8_t STATUS_CHARGING = 1 << 2;
  static const uint8_t STATUS_DISCHARGING = 1 << 3;
  static const uint8_t STATUS_REPLACE = 1 << 4;
  static const uint8_t STATUS_LOW_BATTERY = 1 << 5;
  static const uint8_t STATUS_FULL = 1 << 6;
  static const uint8_t CONVERTER_OVERLOAD = 1 << 4;

  struct HidReport {
    uint8_t report_id{0};
    std::vector<uint8_t> data;
  };

  std::string manufacturer_{"Tripp Lite"};
  std::string model_{"Tripp Lite UPS"};

  bool read_report(uint8_t report_id, HidReport &report);
  size_t payload_offset(const HidReport &report) const;
  bool read_u8(uint8_t report_id, uint8_t &value);
  bool read_u16(uint8_t report_id, uint16_t &value);
  float scale_line_voltage(uint16_t raw) const;
  float scale_frequency(uint16_t raw) const;
  float scale_battery_voltage(uint16_t raw) const;
};

}  // namespace ups_hid
}  // namespace esphome
