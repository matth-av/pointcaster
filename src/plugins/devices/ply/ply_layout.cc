#include "ply_layout.h"

#include <algorithm>
#include <bit>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <cmath>
#include <core/logger/logger.h>
#include <cstring>
#include <miniply.h>
#include <oneapi/tbb/parallel_for.h>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

namespace pc::devices::ply {

namespace {

using miniply::PLYPropertyType;

bool is_floating_point(PLYPropertyType type) {
  return type == PLYPropertyType::Float || type == PLYPropertyType::Double;
}

std::string_view type_name(PLYPropertyType type) {
  switch (type) {
  case PLYPropertyType::Char:
    return "int8";
  case PLYPropertyType::UChar:
    return "uint8";
  case PLYPropertyType::Short:
    return "int16";
  case PLYPropertyType::UShort:
    return "uint16";
  case PLYPropertyType::Int:
    return "int32";
  case PLYPropertyType::UInt:
    return "uint32";
  case PLYPropertyType::Float:
    return "float32";
  case PLYPropertyType::Double:
    return "float64";
  default:
    return "unknown";
  }
}

float position_scale_for(PositionUnits units, bool floating_point_positions) {
  switch (units) {
  case PositionUnits::Millimetres:
    return 1.0f;
  case PositionUnits::Centimetres:
    return 10.0f;
  case PositionUnits::Metres:
    return 1000.0f;
  default:
    return floating_point_positions ? 1000.0f : 1.0f;
  }
}

// colour components arrive as 0..1 floats, 0..255 bytes or 0..65535 shorts
float color_gain_for(PLYPropertyType type) {
  if (is_floating_point(type)) return 255.0f;
  if (type == PLYPropertyType::UShort || type == PLYPropertyType::Short) {
    return 255.0f / 65535.0f;
  }
  return 1.0f;
}

unsigned char to_color_component(float value) {
  return static_cast<unsigned char>(std::clamp(std::lround(value), 0L, 255L));
}

int16_t to_millimetres(float value) {
  return length::from_millimetres(value).mm;
}

template <typename T> T load_scalar(const char *source, bool swap_bytes) {
  T value;
  std::memcpy(&value, source, sizeof(T));
  if (!swap_bytes) return value;
  if constexpr (std::is_floating_point_v<T>) {
    using bit_representation =
        std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    return std::bit_cast<T>(
        std::byteswap(std::bit_cast<bit_representation>(value)));
  } else if constexpr (sizeof(T) > 1) {
    return std::byteswap(value);
  } else {
    return value;
  }
}

float read_value(const char *source, PLYPropertyType type, bool swap_bytes) {
  switch (type) {
  case PLYPropertyType::Char:
    return static_cast<float>(load_scalar<int8_t>(source, swap_bytes));
  case PLYPropertyType::UChar:
    return static_cast<float>(load_scalar<uint8_t>(source, swap_bytes));
  case PLYPropertyType::Short:
    return static_cast<float>(load_scalar<int16_t>(source, swap_bytes));
  case PLYPropertyType::UShort:
    return static_cast<float>(load_scalar<uint16_t>(source, swap_bytes));
  case PLYPropertyType::Int:
    return static_cast<float>(load_scalar<int32_t>(source, swap_bytes));
  case PLYPropertyType::UInt:
    return static_cast<float>(load_scalar<uint32_t>(source, swap_bytes));
  case PLYPropertyType::Float:
    return load_scalar<float>(source, swap_bytes);
  case PLYPropertyType::Double:
    return static_cast<float>(load_scalar<double>(source, swap_bytes));
  default:
    return 0.0f;
  }
}

bool seek_vertex_element(miniply::PLYReader &reader, const std::string &path) {
  if (!reader.valid()) {
    pc::logger()->error("could not open PLY: {}", path);
    return false;
  }
  while (reader.has_element() &&
         !reader.element_is(miniply::kPLYVertexElement)) {
    reader.next_element();
  }
  if (!reader.has_element()) {
    pc::logger()->error("PLY holds no vertex element: {}", path);
    return false;
  }
  return true;
}

struct VertexLayout {
  uint32_t position[3]{};
  uint32_t color[3]{};
  uint32_t alpha = miniply::kInvalidIndex;
  bool has_position = false;
  bool has_color = false;
  std::vector<uint32_t> attributes;

  bool is_reserved(uint32_t index) const {
    if (has_position && (index == position[0] || index == position[1] ||
                         index == position[2])) {
      return true;
    }
    if (has_color &&
        (index == color[0] || index == color[1] || index == color[2])) {
      return true;
    }
    return index == alpha;
  }
};

VertexLayout layout_of(const miniply::PLYReader &reader) {
  const auto *element = reader.element();

  VertexLayout layout;
  layout.has_position = reader.find_pos(layout.position);
  layout.has_color = reader.find_color(layout.color);
  if (layout.has_color) {
    const auto &red_name = element->properties[layout.color[0]].name;
    layout.alpha = element->find_property(red_name == "r" ? "a" : "alpha");
  }

  const auto property_count = static_cast<uint32_t>(element->properties.size());
  for (uint32_t index = 0; index < property_count; index++) {
    if (element->properties[index].countType != PLYPropertyType::None) continue;
    if (layout.is_reserved(index)) continue;
    layout.attributes.push_back(index);
  }
  return layout;
}

size_t data_offset_of(const char *data, size_t length) {
  static constexpr std::string_view sentinel = "end_header\n";
  const std::string_view header(data, std::min(length, size_t{65536}));
  const auto found = header.find(sentinel);
  return found == std::string_view::npos ? 0 : found + sentinel.size();
}

// binary rows sit at a fixed stride, so the values are taken straight out of
// the mapped pages without the file being read into memory first
std::shared_ptr<PointCloud>
read_mapped(const std::string &path, const miniply::PLYReader &reader,
            const VertexLayout &layout, float position_scale,
            const std::shared_ptr<PointCloud> &cloud,
            std::span<const std::span<float>> attribute_values) {
  using namespace boost::interprocess;

  file_mapping mapping;
  try {
    mapping = file_mapping(path.c_str(), read_only);
  } catch (const interprocess_exception &error) {
    pc::logger()->error("mmap failed: {} — {}", path, error.what());
    return nullptr;
  }
  const mapped_region region(mapping, read_only);
  const auto *base = static_cast<const char *>(region.get_address());
  const auto file_size = region.get_size();

  const auto *element = reader.element();
  const size_t vertex_count = element->count;
  const size_t row_stride = element->rowStride;
  const auto data_offset = data_offset_of(base, file_size);

  if (data_offset == 0 || data_offset + vertex_count * row_stride > file_size) {
    pc::logger()->error("truncated PLY: {}", path);
    return nullptr;
  }

  struct Source {
    size_t offset = 0;
    PLYPropertyType type = PLYPropertyType::None;
  };

  const auto &properties = element->properties;
  const auto source_of = [&properties](uint32_t index) {
    return Source{properties[index].offset, properties[index].type};
  };

  Source position_source[3];
  Source color_source[3];
  for (size_t axis = 0; axis < 3; axis++) {
    position_source[axis] = source_of(layout.position[axis]);
    if (layout.has_color) color_source[axis] = source_of(layout.color[axis]);
  }
  const bool has_alpha = layout.alpha != miniply::kInvalidIndex;
  const auto alpha_source = has_alpha ? source_of(layout.alpha) : Source{};

  std::vector<Source> attribute_source;
  attribute_source.reserve(layout.attributes.size());
  for (const auto index : layout.attributes) {
    attribute_source.push_back(source_of(index));
  }

  const auto color_gain =
      layout.has_color ? color_gain_for(color_source[0].type) : 1.0f;
  const auto alpha_gain = has_alpha ? color_gain_for(alpha_source.type) : 1.0f;
  const bool swap_bytes =
      reader.file_type() == miniply::PLYFileType::BinaryBigEndian;
  const auto *vertex_data = base + data_offset;

  if (!layout.has_color) {
    std::ranges::fill(cloud->colors, color{255, 255, 255, 255});
  }

  tbb::parallel_for(
      tbb::blocked_range<size_t>(0, vertex_count),
      [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); i++) {
          const auto *entry = vertex_data + i * row_stride;

          const auto read_source = [&](const Source &source) {
            return read_value(entry + source.offset, source.type, swap_bytes);
          };

          cloud->positions[i] = {
              to_millimetres(read_source(position_source[0]) * position_scale),
              to_millimetres(read_source(position_source[1]) * position_scale),
              to_millimetres(read_source(position_source[2]) * position_scale)};

          if (layout.has_color) {
            cloud->colors[i] = {
                to_color_component(read_source(color_source[0]) * color_gain),
                to_color_component(read_source(color_source[1]) * color_gain),
                to_color_component(read_source(color_source[2]) * color_gain),
                has_alpha
                    ? to_color_component(read_source(alpha_source) * alpha_gain)
                    : static_cast<unsigned char>(255)};
          }

          for (size_t attribute = 0; attribute < attribute_source.size();
               attribute++) {
            attribute_values[attribute][i] =
                read_source(attribute_source[attribute]);
          }
        }
      });

  return cloud;
}

// ascii rows have no fixed width to walk, so miniply parses them
std::shared_ptr<PointCloud>
read_ascii(miniply::PLYReader &reader, const VertexLayout &layout,
           float position_scale, const std::shared_ptr<PointCloud> &cloud,
           std::span<const std::span<float>> attribute_values) {
  if (!reader.load_element()) {
    pc::logger()->error("could not read ascii PLY vertex data");
    return nullptr;
  }

  const auto *element = reader.element();
  const size_t vertex_count = reader.num_rows();
  std::vector<float> values(vertex_count * 3);

  reader.extract_properties(layout.position, 3, PLYPropertyType::Float,
                            values.data());
  for (size_t i = 0; i < vertex_count; i++) {
    cloud->positions[i] = {to_millimetres(values[i * 3] * position_scale),
                           to_millimetres(values[i * 3 + 1] * position_scale),
                           to_millimetres(values[i * 3 + 2] * position_scale)};
  }

  if (layout.has_color) {
    const auto color_gain =
        color_gain_for(element->properties[layout.color[0]].type);
    reader.extract_properties(layout.color, 3, PLYPropertyType::Float,
                              values.data());

    std::vector<float> alpha_values;
    float alpha_gain = 1.0f;
    if (layout.alpha != miniply::kInvalidIndex) {
      alpha_values.resize(vertex_count);
      alpha_gain = color_gain_for(element->properties[layout.alpha].type);
      reader.extract_properties(&layout.alpha, 1, PLYPropertyType::Float,
                                alpha_values.data());
    }

    for (size_t i = 0; i < vertex_count; i++) {
      cloud->colors[i] = {
          to_color_component(values[i * 3] * color_gain),
          to_color_component(values[i * 3 + 1] * color_gain),
          to_color_component(values[i * 3 + 2] * color_gain),
          alpha_values.empty()
              ? static_cast<unsigned char>(255)
              : to_color_component(alpha_values[i] * alpha_gain)};
    }
  } else {
    std::ranges::fill(cloud->colors, color{255, 255, 255, 255});
  }

  for (size_t attribute = 0; attribute < layout.attributes.size();
       attribute++) {
    const auto index = layout.attributes[attribute];
    reader.extract_properties(&index, 1, PLYPropertyType::Float,
                              attribute_values[attribute].data());
  }

  return cloud;
}

void quantise_attribute(PointCloud &cloud, std::string_view name,
                        const AttributeConfiguration &configuration) {
  visit_raw_type(configuration, [&](auto raw_type) {
    using RawType = typename decltype(raw_type)::type;

    const auto values = cloud.get<float>(name);
    if (values.empty()) return;

    const auto encoding = attribute_encoding_for_range<RawType>(
        configuration.range_min.value(), configuration.range_max.value());

    std::vector<RawType> raw(values.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, values.size()),
                      [&](const tbb::blocked_range<size_t> &range) {
                        for (size_t i = range.begin(); i < range.end(); i++) {
                          raw[i] = quantise<RawType>(values[i], encoding);
                        }
                      });

    auto &attribute = cloud.attributes[name];
    attribute.storage = std::move(raw);
    attribute.encoding = encoding;
  });
}

} // namespace

std::optional<FileInfo> scan_file_info(const std::string &path) {
  miniply::PLYReader reader(path.c_str());
  if (!seek_vertex_element(reader, path)) return std::nullopt;

  const auto *element = reader.element();
  const auto layout = layout_of(reader);

  FileInfo info;
  info.vertex_count = element->count;
  info.floating_point_positions =
      layout.has_position &&
      is_floating_point(element->properties[layout.position[0]].type);

  for (const auto index : layout.attributes) {
    const auto &property = element->properties[index];
    info.attributes.push_back(
        {property.name, std::string(type_name(property.type))});
  }

  return info;
}

std::shared_ptr<PointCloud>
read_point_cloud(const std::string &path, PositionUnits units,
                 std::span<const AttributeConfiguration> attributes) {
  miniply::PLYReader reader(path.c_str());
  if (!seek_vertex_element(reader, path)) return nullptr;

  auto layout = layout_of(reader);
  if (!layout.has_position) {
    pc::logger()->error("PLY vertex element is missing x, y or z: {}", path);
    return nullptr;
  }

  const auto *element = reader.element();
  const auto position_scale = position_scale_for(
      units, is_floating_point(element->properties[layout.position[0]].type));

  // a property is only read once it has a target, and the first property to
  // claim a target is the one read into it
  std::vector<uint32_t> targeted_properties;
  std::vector<const AttributeConfiguration *> targeted;
  for (const auto index : layout.attributes) {
    const auto configuration =
        std::ranges::find_if(attributes, [&](const auto &candidate) {
          return candidate.name == element->properties[index].name;
        });
    if (configuration == attributes.end()) continue;
    const auto target = configuration->target.value();
    if (target == AttributeTarget::None) continue;
    if (std::ranges::any_of(targeted, [&](const auto *claimed) {
          return claimed->target.value() == target;
        })) {
      continue;
    }
    targeted_properties.push_back(index);
    targeted.push_back(&*configuration);
  }
  layout.attributes = std::move(targeted_properties);

  auto cloud = std::make_shared<PointCloud>();
  cloud->resize(element->count);

  std::vector<std::span<float>> attribute_values;
  attribute_values.reserve(targeted.size());
  for (const auto *configuration : targeted) {
    attribute_values.push_back(
        cloud->add<float>(cloud_attribute_name(configuration->target.value())));
  }

  if (element->count == 0) return cloud;

  auto filled =
      reader.file_type() == miniply::PLYFileType::ASCII
          ? read_ascii(reader, layout, position_scale, cloud, attribute_values)
          : read_mapped(path, reader, layout, position_scale, cloud,
                        attribute_values);
  if (!filled) return nullptr;

  for (const auto *configuration : targeted) {
    const auto target = configuration->target.value();
    const auto cloud_name = cloud_attribute_name(target);

    // a point scale is a world space radius, so it takes units in 'position'
    // space (mm)
    if (target == AttributeTarget::PointScale && position_scale != 1.0f) {
      for (auto &value : filled->get<float>(cloud_name)) {
        value *= position_scale;
      }
    }

    // any attribute encoding/quantisation to different precision:
    quantise_attribute(*filled, cloud_name, *configuration);
  }
  return filled;
}

} // namespace pc::devices::ply
