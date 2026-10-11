#include "protocol_tripplite.h"
#include "protocol_factory.h"
#include "constants_hid.h"
#include "constants_ups.h"
#include "esphome/core/log.h"
#include "esp_err.h"
#include <cmath>
#include <cstdio>

namespace esphome {
namespace ups_hid {

static const char *const TL_TAG = "ups_hid.tripplite";

std::unique_ptr<UpsProtocolBase> create_tripplite_protocol(UpsHidComponent *parent) {
  return std::make_unique<TrippLiteProtocol>(parent);
}

bool TrippLiteProtocol::read_report(uint8_t report_id, HidReport &report) {
  if (!parent_->is_device_connected()) {
    return false;
  }

  uint8_t buffer[limits::MAX_HID_REPORT_SIZE];
  size_t buffer_len = sizeof(buffer);
  esp_err_t ret = parent_->hid_get_report(HID_REPORT_TYPE_FEATURE, report_id, buffer, &buffer_len,
                                           parent_->get_protocol_timeout());
  if (ret != ESP_OK || buffer_len == 0) {
    if (!parent_->is_device_connected()) {
      return false;
    }
    buffer_len = sizeof(buffer);
    ret = parent_->hid_get_report(HID_REPORT_TYPE_INPUT, report_id, buffer, &buffer_len,
                                   parent_->get_protocol_timeout());
  }
  if (ret != ESP_OK || buffer_len == 0) {
    return false;
  }

  report.report_id = report_id;
  report.data.assign(buffer, buffer + buffer_len);

  char hex[25];
  size_t pos = 0;
  size_t n = report.data.size() < 8 ? report.data.size() : 8;
  for (size_t i = 0; i < n && pos + 3 < sizeof(hex); i++) {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", report.data[i]);
  }
  ESP_LOGD(TL_TAG, "Report 0x%02X (%zu bytes): %s", report_id, report.data.size(), hex);
  return true;
}

size_t TrippLiteProtocol::payload_offset(const HidReport &report) const {
  if (!report.data.empty() && report.data[0] == report.report_id) {
    return 1;
  }
  return 0;
}

bool TrippLiteProtocol::read_u8(uint8_t report_id, uint8_t &value) {
  HidReport report;
  if (!read_report(report_id, report)) {
    return false;
  }
  size_t off = payload_offset(report);
  if (report.data.size() <= off) {
    return false;
  }
  value = report.data[off];
  return true;
}

bool TrippLiteProtocol::read_u16(uint8_t report_id, uint16_t &value) {
  HidReport report;
  if (!read_report(report_id, report)) {
    return false;
  }
  size_t off = payload_offset(report);
  if (report.data.size() < off + 2) {
    return false;
  }
  value = report.data[off] | (static_cast<uint16_t>(report.data[off + 1]) << 8);
  return true;
}

float TrippLiteProtocol::scale_line_voltage(uint16_t raw) const {
  if (raw == 0 || raw == 0xFFFF) {
    return NAN;
  }
  float voltage = raw > 1000 ? static_cast<float>(raw) / 10.0f : static_cast<float>(raw);
  if (voltage < voltage::MIN_VALID_VOLTAGE || voltage > voltage::MAX_VALID_VOLTAGE) {
    return NAN;
  }
  return voltage;
}

float TrippLiteProtocol::scale_frequency(uint16_t raw) const {
  float candidates[3] = {static_cast<float>(raw), static_cast<float>(raw) / 10.0f, static_cast<float>(raw) / 100.0f};
  for (float hz : candidates) {
    if (hz >= FREQUENCY_MIN_VALID && hz <= FREQUENCY_MAX_VALID) {
      return hz;
    }
  }
  return NAN;
}

float TrippLiteProtocol::scale_battery_voltage(uint16_t raw) const {
  // NUT multiplies the 09AE:2009 family by 0.1 (raw 263 → 26.3V on a 24V pack).
  if (raw >= 100 && raw <= 1000) {
    return static_cast<float>(raw) / 10.0f;
  }
  if (raw >= 10 && raw <= 60) {
    return static_cast<float>(raw);
  }
  return NAN;
}

bool TrippLiteProtocol::detect() {
  if (!parent_->is_device_connected()) {
    return false;
  }
  if (parent_->get_vendor_id() != usb::VENDOR_ID_TRIPPLITE) {
    return false;
  }

  ESP_LOGD(TL_TAG, "Detecting Tripp Lite HID for product 0x%04X", parent_->get_product_id());

  // 0x30 is the nominal-voltage byte this 09AE:2009 unit already answers.
  // 0x34 is battery percent on the SMART1500LCD map. Either one is enough
  // to claim the device; missing battery data is logged from read_data.
  uint8_t probe = 0;
  if (read_u8(REPORT_BATTERY_LEVEL, probe) || read_u8(REPORT_INPUT_NOMINAL, probe)) {
    ESP_LOGI(TL_TAG, "Tripp Lite HID protocol detected");
    return true;
  }

  ESP_LOGW(TL_TAG, "Tripp Lite vendor 0x09AE did not answer reports 0x34 or 0x30");
  return false;
}

bool TrippLiteProtocol::initialize() {
  if (parent_->get_vendor_id() != usb::VENDOR_ID_TRIPPLITE) {
    ESP_LOGW(TL_TAG, "Refusing to initialize: vendor 0x%04X is not Tripp Lite", parent_->get_vendor_id());
    return false;
  }

  std::string str;
  if (parent_->get_string_descriptor(3, str) == ESP_OK && !str.empty()) {
    manufacturer_ = str;
  }
  str.clear();
  if (parent_->get_string_descriptor(1, str) == ESP_OK && !str.empty()) {
    model_ = str;
  }

  ESP_LOGI(TL_TAG, "UPS initialized: %s %s (USB 09AE:%04X)", manufacturer_.c_str(), model_.c_str(),
           parent_->get_product_id());
  return true;
}

bool TrippLiteProtocol::read_data(UpsData &data) {
  data.device.manufacturer = manufacturer_;
  data.device.model = model_;
  data.device.usb_vendor_id = parent_->get_vendor_id();
  data.device.usb_product_id = parent_->get_product_id();
  data.device.capabilities.supports_hid_get_report = true;
  data.device.capabilities.supports_runtime_estimation = true;

  bool success = false;

  uint8_t battery = 0;
  if (read_u8(REPORT_BATTERY_LEVEL, battery) && battery <= 100) {
    data.battery.level = battery;
    success = true;
  }

  uint16_t runtime_s = 0;
  if (read_u16(REPORT_RUNTIME, runtime_s) && runtime_s > 0 && runtime_s < 0xFFFF) {
    data.battery.runtime_minutes = static_cast<float>(runtime_s) / 60.0f;
    success = true;
  }

  uint16_t raw = 0;
  float volts = NAN;
  if (read_u16(REPORT_INPUT_VOLTAGE, raw)) {
    volts = scale_line_voltage(raw);
  }
  if (std::isnan(volts) && read_u16(REPORT_LINE_VOLTAGE, raw)) {
    // 0x31 is PresentStatus.Voltage (line), not output. The generic parser
    // publishes this as output voltage.
    volts = scale_line_voltage(raw);
  }
  if (!std::isnan(volts)) {
    data.power.input_voltage = volts;
    success = true;
  }

  if (read_u16(REPORT_OUTPUT_VOLTAGE, raw)) {
    volts = scale_line_voltage(raw);
    if (!std::isnan(volts)) {
      data.power.output_voltage = volts;
      success = true;
    }
  }

  uint8_t load = 0;
  if (read_u8(REPORT_LOAD, load) && load <= 100) {
    data.power.load_percent = load;
    success = true;
  }

  uint8_t nominal = 0;
  if (read_u8(REPORT_INPUT_NOMINAL, nominal) && nominal >= 80 && nominal <= 250) {
    data.power.input_voltage_nominal = nominal;
    data.power.output_voltage_nominal = nominal;
  }

  if (read_u16(REPORT_FREQUENCY, raw)) {
    float hz = scale_frequency(raw);
    if (!std::isnan(hz)) {
      data.power.frequency = hz;
    }
  }

  if (read_u16(REPORT_BATTERY_VOLTAGE, raw)) {
    float battery_v = scale_battery_voltage(raw);
    if (!std::isnan(battery_v)) {
      data.battery.voltage = battery_v;
    }
  }

  if (read_u16(REPORT_BATTERY_VOLTAGE_NOMINAL, raw) && raw >= 6 && raw <= 72) {
    data.battery.voltage_nominal = raw;
  }

  if (read_u16(REPORT_APPARENT_POWER, raw) && raw >= 100 && raw <= 10000) {
    data.power.apparent_power_nominal = raw;
  }

  uint8_t status = 0;
  if (read_u8(REPORT_PRESENT_STATUS, status)) {
    bool ac_present = (status & STATUS_AC_PRESENT) != 0;
    bool charging = (status & STATUS_CHARGING) != 0;
    bool discharging = (status & STATUS_DISCHARGING) != 0;
    bool full = (status & STATUS_FULL) != 0;

    if (!ac_present || discharging) {
      data.power.status = status::ON_BATTERY;
      data.battery.status = battery_status::DISCHARGING;
    } else if (charging && !full) {
      data.power.status = status::ONLINE;
      data.battery.status = battery_status::CHARGING;
    } else if (full) {
      data.power.status = status::ONLINE;
      data.battery.status = battery_status::FULLY_CHARGED;
    } else {
      data.power.status = status::ONLINE;
      data.battery.status = battery_status::NORMAL;
    }

    if ((status & STATUS_LOW_BATTERY) != 0 && !std::isnan(data.battery.level)) {
      data.battery.charge_low = data.battery.level;
    }
    if ((status & STATUS_REPLACE) != 0) {
      data.battery.status += battery_status::REPLACE_BATTERY_SUFFIX;
    }
    success = true;
  }

  uint8_t converter = 0;
  if (read_u8(REPORT_CONVERTER_STATUS, converter)) {
    data.power.overload = (converter & CONVERTER_OVERLOAD) != 0;
    if (data.power.overload) {
      if (data.power.status.empty()) {
        data.power.status = std::string(status::ONLINE) + " - Overload";
      } else {
        data.power.status += " - Overload";
      }
    }
  }

  if (success) {
    ESP_LOGI(TL_TAG, "batt=%.0f%% runtime=%.0fmin in=%.1fV out=%.1fV load=%.0f%% %s",
             std::isnan(data.battery.level) ? -1.0f : data.battery.level,
             std::isnan(data.battery.runtime_minutes) ? -1.0f : data.battery.runtime_minutes,
             std::isnan(data.power.input_voltage) ? -1.0f : data.power.input_voltage,
             std::isnan(data.power.output_voltage) ? -1.0f : data.power.output_voltage,
             std::isnan(data.power.load_percent) ? -1.0f : data.power.load_percent, data.power.status.c_str());
  } else {
    ESP_LOGW(TL_TAG, "No Tripp Lite HID reports returned data");
  }
  return success;
}

}  // namespace ups_hid
}  // namespace esphome

REGISTER_UPS_PROTOCOL_FOR_VENDOR(0x09AE, tripplite_hid_protocol, esphome::ups_hid::create_tripplite_protocol,
                                 "TrippLite", "Tripp Lite Smart LCD HID protocol (09AE:2009 family)", 200);
