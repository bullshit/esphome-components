#pragma once

#include "ups_hid.h"
#include "data_composite.h"
#include "hid_parser.h"

#include <cmath>
#include <string>
#include <map>
#include <utility>
#include <vector>

namespace esphome {
namespace ups_hid {

/**
 * Eaton / MGE HID protocol (USB vendor 0x0463).
 *
 * Unlike the generic protocol, this reads the device's HID report descriptor
 * and locates each value by its HID Power Device usage path (the same paths
 * NUT's mge-hid subdriver uses), so report IDs and byte offsets come from the
 * device itself instead of being guessed.
 */
// Result of the last Eaton/MGE probe, for dump_config().
const std::string &eaton_probe_status();

class EatonHidProtocol : public UpsProtocolBase {
 public:
  explicit EatonHidProtocol(UpsHidComponent *parent) : UpsProtocolBase(parent) {}
  ~EatonHidProtocol() override = default;

  DeviceInfo::DetectedProtocol get_protocol_type() const override { return DeviceInfo::PROTOCOL_EATON_HID; }
  std::string get_protocol_name() const override { return "Eaton/MGE HID"; }

  bool detect() override;
  bool initialize() override;
  bool read_data(UpsData &data) override;

 protected:
  struct ValueRange {
    float min;
    float max;
  };

  bool load_descriptor();
  const uint8_t *get_report(uint8_t type, uint8_t id, size_t &len);
  bool read_field(const HidField *field, int32_t &logical);
  // Physical value of a field, or NAN. Uses the unit exponent when that gives
  // a plausible value, otherwise the plain logical value.
  float read_value(const HidField *field, ValueRange range);
  // 1 / 0 for a flag field, -1 when unavailable.
  int read_flag(const HidField *field);
  std::string read_string(const HidField *field);

  void dump_step();

  HidReportDescriptor descriptor_;
  std::vector<uint8_t> raw_descriptor_;
  bool descriptor_loaded_{false};

  // Per-cycle report cache keyed by (report type, report id).
  std::map<std::pair<uint8_t, uint8_t>, std::vector<uint8_t>> report_cache_;

  // Resolved fields (nullptr when the device does not provide them).
  const HidField *remaining_capacity_{nullptr};
  const HidField *remaining_capacity_limit_{nullptr};
  const HidField *runtime_to_empty_{nullptr};
  const HidField *battery_voltage_{nullptr};
  const HidField *battery_voltage_nominal_{nullptr};
  const HidField *ac_present_{nullptr};
  const HidField *charging_{nullptr};
  const HidField *discharging_{nullptr};
  const HidField *fully_charged_{nullptr};
  const HidField *below_capacity_limit_{nullptr};
  const HidField *shutdown_imminent_{nullptr};
  const HidField *need_replacement_{nullptr};
  const HidField *overload_{nullptr};
  const HidField *input_voltage_{nullptr};
  const HidField *input_frequency_{nullptr};
  const HidField *output_voltage_{nullptr};
  const HidField *percent_load_{nullptr};
  const HidField *low_transfer_{nullptr};
  const HidField *high_transfer_{nullptr};
  const HidField *input_voltage_nominal_{nullptr};
  const HidField *output_voltage_nominal_{nullptr};
  const HidField *apparent_power_nominal_{nullptr};
  const HidField *active_power_nominal_{nullptr};

  std::string manufacturer_;
  std::string model_;
  std::string serial_;

  // Diagnostic dump progress (descriptor hex, then decoded fields).
  size_t dump_desc_pos_{0};
  size_t dump_field_pos_{0};
  bool dump_done_{false};
};

}  // namespace ups_hid
}  // namespace esphome
