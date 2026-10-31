#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace ups_hid {

// One variable field of a HID report, as declared by the report descriptor.
struct HidField {
  uint8_t report_type{0};  // 1 = input, 2 = output, 3 = feature (HID GET_REPORT types)
  uint8_t report_id{0};
  uint16_t bit_offset{0};  // offset within the report payload (after the report ID byte)
  uint8_t bit_size{0};
  int32_t logical_min{0};
  int32_t logical_max{0};
  int32_t physical_min{0};
  int32_t physical_max{0};
  uint32_t unit{0};
  int8_t unit_exponent{0};
  // Usage path: enclosing collection usages followed by the field usage,
  // each as (usage_page << 16) | usage_id.
  std::vector<uint32_t> path;

  uint32_t usage() const { return path.empty() ? 0 : path.back(); }
};

class HidReportDescriptor {
 public:
  // Parses a raw HID report descriptor. Returns false if it is malformed.
  bool parse(const uint8_t *data, size_t len);

  const std::vector<HidField> &fields() const { return fields_; }

  // Finds the first field whose usage path ends with the given suffix and
  // contains every usage of `within` somewhere before it (in order).
  const HidField *find(const std::vector<uint32_t> &suffix, uint8_t report_type = 0,
                       const std::vector<uint32_t> &within = {}) const;

  // Total payload length in bytes (excluding report ID) of a report.
  size_t report_length(uint8_t report_type, uint8_t report_id) const;

  bool uses_report_ids() const { return uses_report_ids_; }

 private:
  std::vector<HidField> fields_;
  bool uses_report_ids_{false};
};

// Extracts the raw logical value of a field from a report buffer. The buffer
// starts with the report ID byte when the device uses report IDs.
bool hid_extract_logical(const HidField &field, const uint8_t *report, size_t report_len, bool has_report_id,
                         int32_t &out);

// Converts a logical value to a physical value (applies physical range scaling
// and the unit exponent).
float hid_logical_to_physical(const HidField &field, int32_t logical);

}  // namespace ups_hid
}  // namespace esphome
