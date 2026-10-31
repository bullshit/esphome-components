#include "protocol_eaton.h"
#include "constants_ups.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace ups_hid {

static const char *const EATON_TAG = "ups_hid.eaton";

static std::string probe_status = "not run";
const std::string &eaton_probe_status() { return probe_status; }

namespace {

constexpr uint32_t usage(uint16_t page, uint16_t id) { return (static_cast<uint32_t>(page) << 16) | id; }

// HID Power Device page (0x84)
constexpr uint32_t PD_PRESENT_STATUS = usage(0x84, 0x02);
constexpr uint32_t PD_UPS = usage(0x84, 0x04);
constexpr uint32_t PD_BATTERY = usage(0x84, 0x12);
constexpr uint32_t PD_POWER_CONVERTER = usage(0x84, 0x16);
constexpr uint32_t PD_INPUT = usage(0x84, 0x1A);
constexpr uint32_t PD_OUTPUT = usage(0x84, 0x1C);
constexpr uint32_t PD_FLOW = usage(0x84, 0x1E);
constexpr uint32_t PD_POWER_SUMMARY = usage(0x84, 0x24);
constexpr uint32_t PD_VOLTAGE = usage(0x84, 0x30);
constexpr uint32_t PD_FREQUENCY = usage(0x84, 0x32);
constexpr uint32_t PD_PERCENT_LOAD = usage(0x84, 0x35);
constexpr uint32_t PD_CONFIG_VOLTAGE = usage(0x84, 0x40);
constexpr uint32_t PD_CONFIG_APPARENT_POWER = usage(0x84, 0x43);
constexpr uint32_t PD_CONFIG_ACTIVE_POWER = usage(0x84, 0x44);
constexpr uint32_t PD_LOW_VOLTAGE_TRANSFER = usage(0x84, 0x53);
constexpr uint32_t PD_HIGH_VOLTAGE_TRANSFER = usage(0x84, 0x54);
constexpr uint32_t PD_OVERLOAD = usage(0x84, 0x65);
constexpr uint32_t PD_SHUTDOWN_IMMINENT = usage(0x84, 0x69);

// Battery System page (0x85)
constexpr uint32_t BS_REMAINING_CAPACITY_LIMIT = usage(0x85, 0x29);
constexpr uint32_t BS_BELOW_REMAINING_CAPACITY_LIMIT = usage(0x85, 0x42);
constexpr uint32_t BS_CHARGING = usage(0x85, 0x44);
constexpr uint32_t BS_DISCHARGING = usage(0x85, 0x45);
constexpr uint32_t BS_FULLY_CHARGED = usage(0x85, 0x46);
constexpr uint32_t BS_NEED_REPLACEMENT = usage(0x85, 0x4B);
constexpr uint32_t BS_REMAINING_CAPACITY = usage(0x85, 0x66);
constexpr uint32_t BS_RUN_TIME_TO_EMPTY = usage(0x85, 0x68);
constexpr uint32_t BS_AC_PRESENT = usage(0x85, 0xD0);
constexpr uint32_t PD_I_MANUFACTURER = usage(0x84, 0xFD);
constexpr uint32_t PD_I_PRODUCT = usage(0x84, 0xFE);
constexpr uint32_t PD_I_SERIAL_NUMBER = usage(0x84, 0xFF);
constexpr uint32_t BS_I_MANUFACTURER = usage(0x85, 0xFD);
constexpr uint32_t BS_I_PRODUCT = usage(0x85, 0xFE);
constexpr uint32_t BS_I_SERIAL_NUMBER = usage(0x85, 0xFF);

constexpr uint8_t REPORT_INPUT = 1;
constexpr uint8_t REPORT_FEATURE = 3;
constexpr uint32_t REPORT_TIMEOUT_MS = 1000;
constexpr size_t REPORT_BUFFER_SIZE = 64;
constexpr size_t DUMP_LINES_PER_CYCLE = 12;

}  // namespace

bool EatonHidProtocol::load_descriptor() {
  if (descriptor_loaded_)
    return true;
  esp_err_t err = parent_->get_report_descriptor(raw_descriptor_);
  if (err != ESP_OK || raw_descriptor_.empty()) {
    probe_status = std::string("report descriptor read failed: ") + esp_err_to_name(err) + " (" +
                   parent_->get_report_descriptor_info() + ")";
    ESP_LOGW(EATON_TAG, "Could not read HID report descriptor: %s", esp_err_to_name(err));
    return false;
  }
  if (!descriptor_.parse(raw_descriptor_.data(), raw_descriptor_.size())) {
    probe_status = "malformed report descriptor (" + std::to_string(raw_descriptor_.size()) + " bytes)";
    ESP_LOGW(EATON_TAG, "Malformed HID report descriptor (%zu bytes)", raw_descriptor_.size());
    return false;
  }
  descriptor_loaded_ = true;
  ESP_LOGI(EATON_TAG, "Parsed HID report descriptor: %zu bytes, %zu fields", raw_descriptor_.size(),
           descriptor_.fields().size());
  return true;
}

bool EatonHidProtocol::detect() {
  ESP_LOGD(EATON_TAG, "Detecting Eaton/MGE HID protocol...");
  if (!parent_->is_connected())
    return false;
  if (!load_descriptor())
    return false;
  // It must be an HID Power Device with a power summary or power converter.
  bool is_ups = descriptor_.find({PD_POWER_SUMMARY, BS_REMAINING_CAPACITY}) != nullptr ||
                descriptor_.find({PD_INPUT, PD_VOLTAGE}) != nullptr;
  probe_status = std::to_string(raw_descriptor_.size()) + " byte descriptor, " +
                 std::to_string(descriptor_.fields().size()) + " fields, " +
                 (is_ups ? "power device usages found" : "no power device usages") + " (" +
                 parent_->get_report_descriptor_info() + ")";
  if (!is_ups)
    ESP_LOGD(EATON_TAG, "Report descriptor has no HID Power Device usages");
  return is_ups;
}

bool EatonHidProtocol::initialize() {
  if (!load_descriptor())
    return false;

  // Prefer Feature reports (always readable with GET_REPORT), then any type.
  auto pick = [this](std::initializer_list<uint32_t> suffix, std::initializer_list<uint32_t> within = {}) {
    std::vector<uint32_t> s(suffix), w(within);
    const HidField *f = descriptor_.find(s, REPORT_FEATURE, w);
    if (f == nullptr)
      f = descriptor_.find(s, 0, w);
    return f;
  };
  // Last match, for values that appear in several flows (output is the last).
  auto pick_last = [this](uint32_t suffix_usage, uint32_t within) {
    const HidField *found = nullptr;
    for (const auto &f : descriptor_.fields()) {
      if (f.usage() != suffix_usage)
        continue;
      for (size_t i = 0; i + 1 < f.path.size(); i++) {
        if (f.path[i] == within) {
          found = &f;
          break;
        }
      }
    }
    return found;
  };

  remaining_capacity_ = pick({PD_POWER_SUMMARY, BS_REMAINING_CAPACITY});
  if (!remaining_capacity_)
    remaining_capacity_ = pick({BS_REMAINING_CAPACITY});
  remaining_capacity_limit_ = pick({PD_POWER_SUMMARY, BS_REMAINING_CAPACITY_LIMIT});
  if (!remaining_capacity_limit_)
    remaining_capacity_limit_ = pick({BS_REMAINING_CAPACITY_LIMIT});
  runtime_to_empty_ = pick({PD_POWER_SUMMARY, BS_RUN_TIME_TO_EMPTY});
  if (!runtime_to_empty_)
    runtime_to_empty_ = pick({BS_RUN_TIME_TO_EMPTY});
  battery_voltage_ = pick({PD_BATTERY, PD_VOLTAGE});
  if (!battery_voltage_)
    battery_voltage_ = pick({PD_POWER_SUMMARY, PD_VOLTAGE});
  battery_voltage_nominal_ = pick({PD_BATTERY, PD_CONFIG_VOLTAGE});
  if (!battery_voltage_nominal_)
    battery_voltage_nominal_ = pick({PD_POWER_SUMMARY, PD_CONFIG_VOLTAGE});

  ac_present_ = pick({PD_PRESENT_STATUS, BS_AC_PRESENT}, {PD_POWER_SUMMARY});
  if (!ac_present_)
    ac_present_ = pick({PD_PRESENT_STATUS, BS_AC_PRESENT});
  charging_ = pick({PD_PRESENT_STATUS, BS_CHARGING}, {PD_POWER_SUMMARY});
  discharging_ = pick({PD_PRESENT_STATUS, BS_DISCHARGING}, {PD_POWER_SUMMARY});
  fully_charged_ = pick({PD_PRESENT_STATUS, BS_FULLY_CHARGED}, {PD_POWER_SUMMARY});
  below_capacity_limit_ = pick({PD_PRESENT_STATUS, BS_BELOW_REMAINING_CAPACITY_LIMIT}, {PD_POWER_SUMMARY});
  shutdown_imminent_ = pick({PD_PRESENT_STATUS, PD_SHUTDOWN_IMMINENT}, {PD_POWER_SUMMARY});
  need_replacement_ = pick({PD_PRESENT_STATUS, BS_NEED_REPLACEMENT}, {PD_POWER_SUMMARY});
  if (!need_replacement_)
    need_replacement_ = pick({PD_PRESENT_STATUS, BS_NEED_REPLACEMENT});
  overload_ = pick({PD_PRESENT_STATUS, PD_OVERLOAD}, {PD_POWER_SUMMARY});
  if (!overload_)
    overload_ = pick({PD_PRESENT_STATUS, PD_OVERLOAD});

  input_voltage_ = pick({PD_INPUT, PD_VOLTAGE}, {PD_POWER_CONVERTER});
  if (!input_voltage_)
    input_voltage_ = pick({PD_INPUT, PD_VOLTAGE});
  input_frequency_ = pick({PD_INPUT, PD_FREQUENCY}, {PD_POWER_CONVERTER});
  if (!input_frequency_)
    input_frequency_ = pick({PD_INPUT, PD_FREQUENCY});
  output_voltage_ = pick({PD_OUTPUT, PD_VOLTAGE}, {PD_POWER_CONVERTER});
  if (!output_voltage_)
    output_voltage_ = pick({PD_OUTPUT, PD_VOLTAGE});
  percent_load_ = pick({PD_OUTPUT, PD_PERCENT_LOAD});
  if (!percent_load_)
    percent_load_ = pick({PD_PERCENT_LOAD});
  low_transfer_ = pick({PD_LOW_VOLTAGE_TRANSFER});
  high_transfer_ = pick({PD_HIGH_VOLTAGE_TRANSFER});

  input_voltage_nominal_ = pick({PD_INPUT, PD_CONFIG_VOLTAGE});
  if (!input_voltage_nominal_)
    input_voltage_nominal_ = pick({PD_FLOW, PD_CONFIG_VOLTAGE});
  output_voltage_nominal_ = pick({PD_OUTPUT, PD_CONFIG_VOLTAGE});
  if (!output_voltage_nominal_)
    output_voltage_nominal_ = pick_last(PD_CONFIG_VOLTAGE, PD_FLOW);
  apparent_power_nominal_ = pick_last(PD_CONFIG_APPARENT_POWER, PD_FLOW);
  if (!apparent_power_nominal_)
    apparent_power_nominal_ = pick({PD_CONFIG_APPARENT_POWER});
  active_power_nominal_ = pick_last(PD_CONFIG_ACTIVE_POWER, PD_FLOW);
  if (!active_power_nominal_)
    active_power_nominal_ = pick({PD_CONFIG_ACTIVE_POWER});

  report_cache_.clear();
  // String indexes live on either the Power Device or the Battery page.
  auto pick_string = [&](uint32_t pd_usage, uint32_t bs_usage) {
    const HidField *f = pick({PD_POWER_SUMMARY, pd_usage});
    return f ? f : pick({PD_POWER_SUMMARY, bs_usage});
  };
  manufacturer_ = read_string(pick_string(PD_I_MANUFACTURER, BS_I_MANUFACTURER));
  model_ = read_string(pick_string(PD_I_PRODUCT, BS_I_PRODUCT));
  serial_ = read_string(pick_string(PD_I_SERIAL_NUMBER, BS_I_SERIAL_NUMBER));
  report_cache_.clear();

  ESP_LOGI(EATON_TAG, "Eaton/MGE HID: '%s' '%s' serial '%s'", manufacturer_.c_str(), model_.c_str(),
           serial_.c_str());
  ESP_LOGI(EATON_TAG, "Mapped: charge=%d runtime=%d status=%d in.V=%d in.Hz=%d out.V=%d load=%d", remaining_capacity_ != nullptr,
           runtime_to_empty_ != nullptr, ac_present_ != nullptr, input_voltage_ != nullptr,
           input_frequency_ != nullptr, output_voltage_ != nullptr, percent_load_ != nullptr);

  return remaining_capacity_ != nullptr || input_voltage_ != nullptr;
}

const uint8_t *EatonHidProtocol::get_report(uint8_t type, uint8_t id, size_t &len) {
  auto key = std::make_pair(type, id);
  auto it = report_cache_.find(key);
  if (it == report_cache_.end()) {
    std::vector<uint8_t> buffer(REPORT_BUFFER_SIZE, 0);
    size_t buffer_len = buffer.size();
    if (parent_->hid_get_report(type, id, buffer.data(), &buffer_len, REPORT_TIMEOUT_MS) != ESP_OK)
      buffer_len = 0;
    buffer.resize(buffer_len);
    it = report_cache_.emplace(key, std::move(buffer)).first;
  }
  len = it->second.size();
  return len > 0 ? it->second.data() : nullptr;
}

bool EatonHidProtocol::read_field(const HidField *field, int32_t &logical) {
  if (field == nullptr)
    return false;
  size_t len = 0;
  const uint8_t *report = get_report(field->report_type, field->report_id, len);
  if (report == nullptr)
    return false;
  bool has_id = descriptor_.uses_report_ids() && report[0] == field->report_id;
  return hid_extract_logical(*field, report, len, has_id, logical);
}

float EatonHidProtocol::read_value(const HidField *field, ValueRange range) {
  int32_t logical;
  if (!read_field(field, logical))
    return NAN;
  float scaled = hid_logical_to_physical(*field, logical);
  if (scaled >= range.min && scaled <= range.max)
    return scaled;
  // Some firmware declares unit exponents that do not match the data.
  HidField plain = *field;
  plain.unit_exponent = 0;
  float unscaled = hid_logical_to_physical(plain, logical);
  if (unscaled >= range.min && unscaled <= range.max)
    return unscaled;
  return NAN;
}

int EatonHidProtocol::read_flag(const HidField *field) {
  int32_t logical;
  if (!read_field(field, logical))
    return -1;
  return logical != 0 ? 1 : 0;
}

std::string EatonHidProtocol::read_string(const HidField *field) {
  int32_t index;
  if (!read_field(field, index) || index <= 0 || index > 255)
    return "";
  std::string result;
  if (parent_->get_string_descriptor(static_cast<uint8_t>(index), result) != ESP_OK)
    return "";
  return result;
}

bool EatonHidProtocol::read_data(UpsData &data) {
  report_cache_.clear();

  float charge = read_value(remaining_capacity_, {0.0f, 100.0f});
  float charge_limit = read_value(remaining_capacity_limit_, {0.0f, 100.0f});
  float runtime_s = read_value(runtime_to_empty_, {0.0f, 7.0f * 86400.0f});
  float battery_v = read_value(battery_voltage_, {1.0f, 500.0f});
  float battery_v_nominal = read_value(battery_voltage_nominal_, {1.0f, 500.0f});
  float in_v = read_value(input_voltage_, {0.0f, 300.0f});
  float in_hz = read_value(input_frequency_, {40.0f, 70.0f});
  float out_v = read_value(output_voltage_, {0.0f, 300.0f});
  float load = read_value(percent_load_, {0.0f, 200.0f});
  int ac = read_flag(ac_present_);
  int chrg = read_flag(charging_);
  int dischrg = read_flag(discharging_);
  int full = read_flag(fully_charged_);
  int below_limit = read_flag(below_capacity_limit_);
  int imminent = read_flag(shutdown_imminent_);
  int replace = read_flag(need_replacement_);
  int overload = read_flag(overload_);

  bool success = !std::isnan(charge) || !std::isnan(in_v) || ac >= 0;
  if (!success) {
    ESP_LOGW(EATON_TAG, "No readable data from UPS");
    return false;
  }

  // Battery
  data.battery.level = charge;
  if (!std::isnan(runtime_s))
    data.battery.runtime_minutes = runtime_s / 60.0f;
  data.battery.voltage = battery_v;
  data.battery.voltage_nominal = battery_v_nominal;
  data.battery.charge_low = charge_limit;
  // Report the UPS's own low-battery decision even above the configured limit.
  if ((below_limit == 1 || imminent == 1) && !std::isnan(charge))
    data.battery.charge_low = std::isnan(charge_limit) ? charge : std::max(charge_limit, charge);

  if (chrg == 1)
    data.battery.status = battery_status::CHARGING;
  else if (dischrg == 1)
    data.battery.status = battery_status::DISCHARGING;
  else if (full == 1 || (!std::isnan(charge) && charge >= 100.0f))
    data.battery.status = battery_status::FULLY_CHARGED;
  else
    data.battery.status = battery_status::NORMAL;
  if (replace == 1)
    data.battery.status += battery_status::REPLACE_BATTERY_SUFFIX;

  // Power. Online/on-battery is derived from a valid input voltage downstream,
  // so clear it when the UPS says mains is gone.
  bool on_battery = ac == 0 || (ac < 0 && dischrg == 1);
  if (on_battery) {
    data.power.status = status::ON_BATTERY;
    data.power.input_voltage = NAN;
  } else {
    data.power.status = status::ONLINE;
    data.power.input_voltage = in_v;
    if (std::isnan(in_v) && ac == 1)
      data.power.input_voltage = parent_->get_fallback_nominal_voltage();
  }
  if (overload == 1)
    data.power.status += " - Overload";
  data.power.frequency = in_hz;
  data.power.output_voltage = out_v;
  data.power.load_percent = load;
  data.power.input_transfer_low = read_value(low_transfer_, {50.0f, 300.0f});
  data.power.input_transfer_high = read_value(high_transfer_, {50.0f, 300.0f});
  data.power.input_voltage_nominal = read_value(input_voltage_nominal_, {50.0f, 300.0f});
  data.power.output_voltage_nominal = read_value(output_voltage_nominal_, {50.0f, 300.0f});
  data.power.apparent_power_nominal = read_value(apparent_power_nominal_, {10.0f, 100000.0f});
  data.power.realpower_nominal = read_value(active_power_nominal_, {10.0f, 100000.0f});

  // Device
  data.device.manufacturer = manufacturer_.empty() ? "Eaton" : manufacturer_;
  if (!model_.empty())
    data.device.model = model_;
  if (!serial_.empty())
    data.device.serial_number = serial_;
  data.device.detected_protocol = DeviceInfo::PROTOCOL_EATON_HID;

  ESP_LOGD(EATON_TAG,
           "charge=%.0f%% runtime=%.0fs batt=%.1fV in=%.1fV %.1fHz out=%.1fV load=%.0f%% ac=%d chrg=%d dischrg=%d "
           "lb=%d",
           charge, runtime_s, battery_v, in_v, in_hz, out_v, load, ac, chrg, dischrg, below_limit);

  dump_step();
  return true;
}

// Logs the raw descriptor and every decoded field, a few lines per poll so the
// API log stream is not flooded. Useful for mapping new models.
void EatonHidProtocol::dump_step() {
  if (dump_done_)
    return;
  size_t lines = 0;
  char line[200];

  while (lines < DUMP_LINES_PER_CYCLE && dump_desc_pos_ < raw_descriptor_.size()) {
    size_t n = std::min<size_t>(32, raw_descriptor_.size() - dump_desc_pos_);
    int w = snprintf(line, sizeof(line), "DESC %04zx:", dump_desc_pos_);
    for (size_t i = 0; i < n && w > 0 && static_cast<size_t>(w) < sizeof(line) - 3; i++)
      w += snprintf(line + w, sizeof(line) - w, " %02x", raw_descriptor_[dump_desc_pos_ + i]);
    ESP_LOGI(EATON_TAG, "%s", line);
    dump_desc_pos_ += n;
    lines++;
  }

  const auto &fields = descriptor_.fields();
  while (lines < DUMP_LINES_PER_CYCLE && dump_field_pos_ < fields.size()) {
    const HidField &f = fields[dump_field_pos_];
    int32_t logical = 0;
    bool ok = read_field(&f, logical);
    int w = snprintf(line, sizeof(line), "FIELD %03zu t=%u id=%02x off=%u sz=%u lg=[%" PRId32 ",%" PRId32
                     "] ph=[%" PRId32 ",%" PRId32 "] u=%" PRIx32 " e=%d v=%s%" PRId32 " ",
                     dump_field_pos_, f.report_type, f.report_id, f.bit_offset, f.bit_size, f.logical_min,
                     f.logical_max, f.physical_min, f.physical_max, f.unit, f.unit_exponent, ok ? "" : "?", logical);
    for (size_t i = 0; i < f.path.size() && w > 0 && static_cast<size_t>(w) < sizeof(line) - 12; i++)
      w += snprintf(line + w, sizeof(line) - w, "%s%" PRIx32, i ? "." : "", f.path[i]);
    ESP_LOGI(EATON_TAG, "%s", line);
    dump_field_pos_++;
    lines++;
  }

  if (dump_desc_pos_ >= raw_descriptor_.size() && dump_field_pos_ >= fields.size()) {
    ESP_LOGI(EATON_TAG, "Descriptor dump complete");
    dump_done_ = true;
  }
}

std::unique_ptr<UpsProtocolBase> create_eaton_protocol(UpsHidComponent *parent) {
  return std::make_unique<EatonHidProtocol>(parent);
}

}  // namespace ups_hid
}  // namespace esphome

#include "protocol_factory.h"

REGISTER_UPS_PROTOCOL_FOR_VENDOR(0x0463, eaton_hid_protocol, esphome::ups_hid::create_eaton_protocol,
                                 "Eaton/MGE HID Protocol",
                                 "Eaton/MGE HID Power Device protocol driven by the report descriptor", 100);
