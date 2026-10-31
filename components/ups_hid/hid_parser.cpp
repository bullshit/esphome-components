#include "hid_parser.h"

#include <cmath>
#include <map>
#include <utility>

namespace esphome {
namespace ups_hid {

namespace {

struct GlobalState {
  uint16_t usage_page{0};
  int32_t logical_min{0};
  int32_t logical_max{0};
  int32_t physical_min{0};
  int32_t physical_max{0};
  int8_t unit_exponent{0};
  uint32_t unit{0};
  uint8_t report_size{0};
  uint8_t report_id{0};
  uint16_t report_count{0};
};

int32_t sign_extend(uint32_t value, uint8_t size_bytes) {
  switch (size_bytes) {
    case 1:
      return static_cast<int8_t>(value);
    case 2:
      return static_cast<int16_t>(value);
    default:
      return static_cast<int32_t>(value);
  }
}

uint32_t full_usage(uint32_t value, uint8_t size_bytes, uint16_t usage_page) {
  if (size_bytes == 4)
    return value;
  return (static_cast<uint32_t>(usage_page) << 16) | (value & 0xFFFF);
}

}  // namespace

bool HidReportDescriptor::parse(const uint8_t *data, size_t len) {
  fields_.clear();
  uses_report_ids_ = false;

  GlobalState global;
  std::vector<GlobalState> global_stack;
  std::vector<uint32_t> usages;
  bool have_usage_min = false;
  uint32_t usage_min = 0;
  std::vector<uint32_t> collections;
  // Next free bit offset per (report_type, report_id).
  std::map<std::pair<uint8_t, uint8_t>, uint16_t> offsets;

  size_t pos = 0;
  while (pos < len) {
    uint8_t prefix = data[pos++];
    if (prefix == 0xFE) {  // long item: skip
      if (pos + 2 > len)
        return false;
      uint8_t data_size = data[pos];
      pos += 2 + data_size;
      continue;
    }
    uint8_t size = prefix & 0x03;
    if (size == 3)
      size = 4;
    uint8_t type = (prefix >> 2) & 0x03;
    uint8_t tag = prefix >> 4;
    if (pos + size > len)
      return false;
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++)
      value |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
    pos += size;

    if (type == 0) {  // Main
      if (tag == 0x8 || tag == 0x9 || tag == 0xB) {
        uint8_t report_type = tag == 0x8 ? 1 : (tag == 0x9 ? 2 : 3);
        auto key = std::make_pair(report_type, global.report_id);
        uint16_t &offset = offsets[key];
        bool is_constant = value & 0x01;
        bool is_variable = value & 0x02;
        for (uint16_t i = 0; i < global.report_count; i++) {
          uint32_t usage = 0;
          if (!usages.empty())
            usage = i < usages.size() ? usages[i] : usages.back();
          if (is_variable && !(is_constant && usages.empty()) && usage != 0) {
            HidField field;
            field.report_type = report_type;
            field.report_id = global.report_id;
            field.bit_offset = offset;
            field.bit_size = global.report_size;
            field.logical_min = global.logical_min;
            field.logical_max = global.logical_max;
            field.physical_min = global.physical_min;
            field.physical_max = global.physical_max;
            field.unit = global.unit;
            field.unit_exponent = global.unit_exponent;
            field.path = collections;
            field.path.push_back(usage);
            fields_.push_back(std::move(field));
          }
          offset += global.report_size;
        }
        usages.clear();
        have_usage_min = false;
      } else if (tag == 0xA) {  // Collection
        collections.push_back(usages.empty() ? 0 : usages.front());
        usages.clear();
        have_usage_min = false;
      } else if (tag == 0xC) {  // End Collection
        if (!collections.empty())
          collections.pop_back();
        usages.clear();
        have_usage_min = false;
      }
    } else if (type == 1) {  // Global
      switch (tag) {
        case 0x0:
          global.usage_page = value;
          break;
        case 0x1:
          global.logical_min = sign_extend(value, size);
          break;
        case 0x2:
          global.logical_max = sign_extend(value, size);
          break;
        case 0x3:
          global.physical_min = sign_extend(value, size);
          break;
        case 0x4:
          global.physical_max = sign_extend(value, size);
          break;
        case 0x5:
          // Spec: 4-bit signed nibble. Some devices send a full signed byte.
          if (size == 1 && value > 0x0F)
            global.unit_exponent = static_cast<int8_t>(value);
          else
            global.unit_exponent = (value & 0x08) ? static_cast<int8_t>((value & 0x0F) | 0xF0)
                                                  : static_cast<int8_t>(value & 0x0F);
          break;
        case 0x6:
          global.unit = value;
          break;
        case 0x7:
          global.report_size = value;
          break;
        case 0x8:
          global.report_id = value;
          uses_report_ids_ = true;
          break;
        case 0x9:
          global.report_count = value;
          break;
        case 0xA:
          global_stack.push_back(global);
          break;
        case 0xB:
          if (!global_stack.empty()) {
            global = global_stack.back();
            global_stack.pop_back();
          }
          break;
        default:
          break;
      }
    } else if (type == 2) {  // Local
      if (tag == 0x0) {
        usages.push_back(full_usage(value, size, global.usage_page));
      } else if (tag == 0x1) {
        usage_min = full_usage(value, size, global.usage_page);
        have_usage_min = true;
      } else if (tag == 0x2 && have_usage_min) {
        uint32_t usage_max = full_usage(value, size, global.usage_page);
        for (uint32_t u = usage_min; u <= usage_max && usages.size() < 256; u++)
          usages.push_back(u);
        have_usage_min = false;
      }
    }
  }
  return true;
}

const HidField *HidReportDescriptor::find(const std::vector<uint32_t> &suffix, uint8_t report_type,
                                          const std::vector<uint32_t> &within) const {
  for (const auto &field : fields_) {
    if (report_type != 0 && field.report_type != report_type)
      continue;
    if (field.path.size() < suffix.size())
      continue;
    size_t start = field.path.size() - suffix.size();
    bool match = true;
    for (size_t i = 0; i < suffix.size() && match; i++)
      match = field.path[start + i] == suffix[i];
    if (!match)
      continue;
    // Every `within` usage must appear, in order, before the suffix.
    size_t next = 0;
    for (size_t i = 0; i < start && next < within.size(); i++) {
      if (field.path[i] == within[next])
        next++;
    }
    if (next == within.size())
      return &field;
  }
  return nullptr;
}

size_t HidReportDescriptor::report_length(uint8_t report_type, uint8_t report_id) const {
  uint32_t bits = 0;
  for (const auto &field : fields_) {
    if (field.report_type == report_type && field.report_id == report_id) {
      uint32_t end = field.bit_offset + field.bit_size;
      if (end > bits)
        bits = end;
    }
  }
  return (bits + 7) / 8;
}

bool hid_extract_logical(const HidField &field, const uint8_t *report, size_t report_len, bool has_report_id,
                         int32_t &out) {
  size_t base = has_report_id ? 1 : 0;
  size_t first_bit = field.bit_offset;
  size_t last_bit = first_bit + field.bit_size;  // exclusive
  if (field.bit_size == 0 || field.bit_size > 32 || base + (last_bit + 7) / 8 > report_len)
    return false;

  uint32_t raw = 0;
  for (size_t bit = first_bit; bit < last_bit; bit++) {
    uint8_t byte = report[base + bit / 8];
    if (byte & (1u << (bit % 8)))
      raw |= 1u << (bit - first_bit);
  }

  if (field.logical_min < 0 && field.bit_size < 32 && (raw & (1u << (field.bit_size - 1))))
    raw |= ~((1u << field.bit_size) - 1);
  out = static_cast<int32_t>(raw);
  return true;
}

float hid_logical_to_physical(const HidField &field, int32_t logical) {
  double value = logical;
  int64_t log_max = field.logical_max;
  // Devices often declare an unsigned maximum that sign-extends negative.
  if (log_max < field.logical_min && field.bit_size < 32)
    log_max = static_cast<int64_t>(field.logical_max) & ((1LL << field.bit_size) - 1);
  bool has_physical = field.physical_min != 0 || field.physical_max != 0;
  if (has_physical && field.physical_max > field.physical_min && log_max > field.logical_min) {
    double factor =
        static_cast<double>(field.physical_max - field.physical_min) / static_cast<double>(log_max - field.logical_min);
    value = field.physical_min + (logical - field.logical_min) * factor;
  }
  // HID units are CGS based: in the SI linear/rotation systems a length is in
  // cm and a mass in g, so e.g. a Volt (g*cm^2/(s^3*A)) needs exponent 7 to
  // be 1 V. Correct for that so values come out in SI units (V, W, VA, A).
  int exponent = field.unit_exponent;
  uint8_t system = field.unit & 0x0F;
  if (system == 1 || system == 2) {
    auto nibble = [&](int index) {
      int n = (field.unit >> (4 * index)) & 0x0F;
      return n > 7 ? n - 16 : n;
    };
    exponent -= 2 * nibble(1) + 3 * nibble(2);
  }
  if (exponent != 0)
    value *= std::pow(10.0, exponent);
  return static_cast<float>(value);
}

}  // namespace ups_hid
}  // namespace esphome
