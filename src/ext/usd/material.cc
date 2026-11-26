// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "material.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix3f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/imaging/hd/dataSourceTypeDefs.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/ar/packageUtils.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usdShade/udimUtils.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/ext/ocio/ocio.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// The reflectance of umbra's diffuse materials: 18% gray.
constexpr float kGray = 0.18F;

// The MaterialX nodedef of a UsdPreviewSurface node, as MaterialX's
// libraries/bxdf/usd_preview_surface.mtlx names it.
std::optional<std::string> PreviewSurfaceNodeDef(std::string_view shader_id) {
  static constexpr auto kNodeDefs =
      std::to_array<std::pair<std::string_view, std::string_view>>({
          {"UsdPreviewSurface", "ND_UsdPreviewSurface_surfaceshader"},
          {"UsdUVTexture", "ND_UsdUVTexture"},
          {"UsdTransform2d", "ND_UsdTransform2d"},
          {"UsdPrimvarReader_float", "ND_UsdPrimvarReader_float"},
          {"UsdPrimvarReader_float2", "ND_UsdPrimvarReader_vector2"},
          {"UsdPrimvarReader_float3", "ND_UsdPrimvarReader_vector3"},
          {"UsdPrimvarReader_float4", "ND_UsdPrimvarReader_vector4"},
          {"UsdPrimvarReader_normal", "ND_UsdPrimvarReader_vector3"},
          {"UsdPrimvarReader_point", "ND_UsdPrimvarReader_vector3"},
          {"UsdPrimvarReader_vector", "ND_UsdPrimvarReader_vector3"},
          {"UsdPrimvarReader_int", "ND_UsdPrimvarReader_integer"},
          {"UsdPrimvarReader_string", "ND_UsdPrimvarReader_string"},
          {"UsdPrimvarReader_matrix", "ND_UsdPrimvarReader_matrix44"},
          {"UsdPrimvarReader_bool", "ND_UsdPrimvarReader_boolean"},
      });
  for (const auto& [usd, nodedef] : kNodeDefs) {
    if (usd == shader_id) {
      return std::string(nodedef);
    }
  }
  return std::nullopt;
}

// The MaterialX nodedef of `shader_id`: a nodedef as it is, or
// UsdPreviewSurface's; none for another.
std::optional<std::string> NodeDef(std::string_view shader_id) {
  if (shader_id.starts_with("ND_")) {
    return std::string(shader_id);
  }
  return PreviewSurfaceNodeDef(shader_id);
}

// Whether `type_name` ("color3f", "color3f[]") is a type of the color role.
bool IsColorType(const pxr::TfToken& type_name) {
  if (type_name.IsEmpty()) {
    return false;
  }
  return pxr::SdfSchema::GetInstance().FindType(type_name).GetRole() ==
         pxr::SdfValueRoleNames->Color;
}

// The components of a Gf vector, as floats, read from its named fields.
template <std::size_t N, typename Vector>
  requires(Vector::dimension == N)
std::array<float, N> Floats(const Vector& vector) {
  if constexpr (N == 2) {
    return {static_cast<float>(vector.x), static_cast<float>(vector.y)};
  } else if constexpr (N == 3) {
    return {
        static_cast<float>(vector.x),
        static_cast<float>(vector.y),
        static_cast<float>(vector.z),
    };
  } else {
    return {
        static_cast<float>(vector.x),
        static_cast<float>(vector.y),
        static_cast<float>(vector.z),
        static_cast<float>(vector.w),
    };
  }
}

// The components of a square Gf matrix, as floats, row-major.
template <std::size_t N, typename Matrix>
  requires(Matrix::numRows * Matrix::numColumns == N)
std::array<float, N> Floats(const Matrix& matrix) {
  constexpr std::size_t kRows = Matrix::numRows;
  std::array<float, N> floats{};
  for (std::size_t row = 0; row < kRows; ++row) {
    const std::array<float, kRows> values =
        Floats<kRows>(matrix.GetRow(static_cast<int>(row)));
    for (std::size_t column = 0; column < kRows; ++column) {
      floats.at((row * kRows) + column) = values.at(column);
    }
  }
  return floats;
}

// The value of `value`, a Gf vector or matrix of N components of either
// `Single` or `Double` precision, as floats; none for another type.
template <std::size_t N, typename Single, typename Double>
std::optional<scene::ParameterValue> GfValue(const pxr::VtValue& value) {
  if (value.IsHolding<Single>()) {
    return Floats<N>(value.UncheckedGet<Single>());
  }
  if (value.IsHolding<Double>()) {
    return Floats<N>(value.UncheckedGet<Double>());
  }
  return std::nullopt;
}

// The elements of a VtArray of T, as a vector of U.
template <typename U, typename T>
std::vector<U> Elements(const pxr::VtArray<T>& array) {
  std::vector<U> elements;
  elements.reserve(array.size());
  for (const T& element : array) {
    if constexpr (std::is_same_v<T, pxr::TfToken>) {
      elements.push_back(element.GetString());
    } else {
      elements.emplace_back(element);
    }
  }
  return elements;
}

// A scalar or a string; none for another type.
std::optional<scene::ParameterValue> ScalarValue(const pxr::VtValue& value) {
  if (value.IsHolding<bool>()) {
    return value.UncheckedGet<bool>();
  }
  if (value.IsHolding<int>()) {
    return std::int32_t{value.UncheckedGet<int>()};
  }
  if (value.IsHolding<float>()) {
    return value.UncheckedGet<float>();
  }
  if (value.IsHolding<double>()) {
    return static_cast<float>(value.UncheckedGet<double>());
  }
  if (value.IsHolding<pxr::TfToken>()) {
    return value.UncheckedGet<pxr::TfToken>().GetString();
  }
  if (value.IsHolding<std::string>()) {
    return value.UncheckedGet<std::string>();
  }
  return std::nullopt;
}

// An array of integers, floats or strings; none for another type.
std::optional<scene::ParameterValue> ArrayValue(const pxr::VtValue& value) {
  if (value.IsHolding<pxr::VtIntArray>()) {
    return Elements<std::int32_t>(value.UncheckedGet<pxr::VtIntArray>());
  }
  if (value.IsHolding<pxr::VtFloatArray>()) {
    return Elements<float>(value.UncheckedGet<pxr::VtFloatArray>());
  }
  if (value.IsHolding<pxr::VtStringArray>()) {
    return Elements<std::string>(value.UncheckedGet<pxr::VtStringArray>());
  }
  if (value.IsHolding<pxr::VtTokenArray>()) {
    return Elements<std::string>(value.UncheckedGet<pxr::VtTokenArray>());
  }
  return std::nullopt;
}

// The value of a parameter that is neither a color nor an asset; none for a
// type umbra does not take.
std::optional<scene::ParameterValue> PlainValue(const pxr::VtValue& value) {
  using Reader = std::optional<scene::ParameterValue> (*)(const pxr::VtValue&);
  static constexpr std::array<Reader, 7> kReaders = {
      ScalarValue,
      GfValue<2, pxr::GfVec2f, pxr::GfVec2d>,
      GfValue<3, pxr::GfVec3f, pxr::GfVec3d>,
      GfValue<4, pxr::GfVec4f, pxr::GfVec4d>,
      GfValue<9, pxr::GfMatrix3f, pxr::GfMatrix3d>,
      GfValue<16, pxr::GfMatrix4f, pxr::GfMatrix4d>,
      ArrayValue,
  };
  for (const Reader reader : kReaders) {
    if (std::optional<scene::ParameterValue> read = reader(value);
        read.has_value()) {
      return read;
    }
  }
  return std::nullopt;
}

// Whether a color parameter is one color or an array of them.
enum class ColorShape : std::uint8_t {
  kSingle,
  kArray,
};

// The numbers of a color parameter, to convert.
struct ColorNumbers {
  // Its RGB triples, those of a color array one after another.
  std::vector<float> rgb;
  // The alpha of a color4.
  std::optional<float> alpha;
  ColorShape shape = ColorShape::kSingle;
};

// The numbers of `value`, a color3, a color4 or a color3 array; none for a
// value of another type.
std::optional<ColorNumbers> ColorComponents(const pxr::VtValue& value) {
  if (auto rgb = GfValue<3, pxr::GfVec3f, pxr::GfVec3d>(value)) {
    const auto& triple = std::get<std::array<float, 3>>(*rgb);
    return ColorNumbers{
        .rgb = {triple.begin(), triple.end()},
        .alpha = std::nullopt,
        .shape = ColorShape::kSingle,
    };
  }
  if (auto rgba = GfValue<4, pxr::GfVec4f, pxr::GfVec4d>(value)) {
    const auto& quad = std::get<std::array<float, 4>>(*rgba);
    return ColorNumbers{
        .rgb = {quad.at(0), quad.at(1), quad.at(2)},
        .alpha = quad.at(3),
        .shape = ColorShape::kSingle,
    };
  }
  if (value.IsHolding<pxr::VtVec3fArray>()) {
    ColorNumbers numbers{
        .rgb = {},
        .alpha = std::nullopt,
        .shape = ColorShape::kArray,
    };
    for (const pxr::GfVec3f& color : value.UncheckedGet<pxr::VtVec3fArray>()) {
      const std::array<float, 3> triple = Floats<3>(color);
      numbers.rgb.insert(numbers.rgb.end(), triple.begin(), triple.end());
    }
    return numbers;
  }
  return std::nullopt;
}

// The parameter value of `numbers`, converted.
scene::ParameterValue ColorValue(ColorNumbers numbers) {
  if (numbers.shape == ColorShape::kArray) {
    return std::move(numbers.rgb);
  }
  const std::array<float, 3> rgb = {
      numbers.rgb.at(0),
      numbers.rgb.at(1),
      numbers.rgb.at(2),
  };
  if (numbers.alpha.has_value()) {
    return scene::ColorAlpha{.rgb = rgb, .alpha = *numbers.alpha};
  }
  return scene::Color{.rgb = rgb};
}

// The parameter value of `color`, converted into ACEScg from `color_space`,
// kDefaultColorSpace when empty.
std::expected<std::optional<scene::ParameterValue>, std::string> ConvertedColor(
    ColorNumbers color, const pxr::TfToken& color_space) {
  if (auto converted = ocio::ConvertToAcescg(
          color_space.IsEmpty() ? kDefaultColorSpace : color_space.GetString(),
          color.rgb);
      !converted.has_value()) {
    return std::unexpected(std::string(converted.error().message()));
  }
  return ColorValue(std::move(color));
}

// The first file of `file`: the first tile of a UDIM set, the file itself
// otherwise; none when a UDIM set has no tile.
std::optional<std::string> FirstFile(const std::string& file) {
  if (!pxr::UsdShadeUdimUtils::IsUdimIdentifier(file)) {
    return file;
  }
  const auto tiles =
      pxr::UsdShadeUdimUtils::ResolveUdimTilePaths(file, pxr::SdfLayerHandle());
  if (tiles.empty()) {
    return std::nullopt;
  }
  return tiles.front().first;
}

// Whether the texels of `file` are sRGB, by USD's convention: as its
// metadata says, or else when it is 8-bit with 3 or 4 channels.
bool IsSrgb(const std::string& file) {
  const std::optional<std::string> first = FirstFile(file);
  if (!first.has_value()) {
    return false;
  }
  const pxr::HioImageSharedPtr image = pxr::HioImage::OpenForReading(*first);
  return image != nullptr && image->IsColorSpaceSRGB();
}

// The color space of the texture `file` of a node of `shader`, by its name
// in the OpenColorIO config: the one authored, else auto for
// ND_UsdUVTexture, as UsdPreviewSurface defines, and kDefaultColorSpace for
// other nodes. auto becomes sRGB or raw, as the file tells.
std::expected<std::string, std::string> TextureColorSpace(
    const pxr::TfToken& authored, const std::string& file,
    std::string_view shader) {
  std::string name = authored.GetString();
  if (name.empty()) {
    name = shader == "ND_UsdUVTexture" ? "auto" : kDefaultColorSpace;
  }
  if (name == "auto") {
    name = IsSrgb(file) ? "sRGB" : "raw";
  }
  // UsdUVTexture's sourceColorSpace names sRGB as no config does.
  if (name == "sRGB") {
    name = "srgb_texture";
  }
  return ocio::ColorSpaceName(name).transform_error(
      [](const Error& error) { return std::string(error.message()); });
}

// Translates the nodes of one network, gathering the textures that do not
// resolve and the error that stops the render.
class Translator {
 public:
  explicit Translator(
      const pxr::HdMaterialNetworkInterface& network UMBRA_LIFETIMEBOUND)
      : network_(&network) {}

  std::expected<TranslatedMaterial, Error> Translate();

 private:
  // The nodes the surface reaches through its connections, itself first.
  [[nodiscard]] pxr::TfTokenVector SurfaceNodes() const;
  // The nodes the inputs of the node `name` connect to.
  [[nodiscard]] pxr::TfTokenVector Upstream(const pxr::TfToken& name) const;
  std::expected<scene::MaterialNetwork, std::string> Network(
      const pxr::TfTokenVector& names);
  // The output the surface is; fails when there is none.
  std::expected<scene::NodeOutput, std::string> Surface();
  // The path of the prim of the node `name`.
  [[nodiscard]] pxr::SdfPath NodePath(const pxr::TfToken& name) const;
  std::expected<scene::MaterialNode, std::string> Node(
      const pxr::TfToken& name);
  [[nodiscard]] std::expected<std::vector<scene::Connection>, std::string>
  Connections(const pxr::TfToken& name) const;
  // The connections of the input `input` of the node `name`.
  [[nodiscard]] std::expected<std::vector<scene::Connection>, std::string>
  Connect(const pxr::TfToken& name, const pxr::TfToken& input) const;
  // Gives `node` its kind and its shader.
  std::expected<void, std::string> AssignShader(const pxr::TfToken& name,
                                                scene::MaterialNode& node);
  std::expected<void, std::string> AssignOslShader(const pxr::TfToken& name,
                                                   scene::MaterialNode& node);
  // The value of the parameter `param` of the node `name`; none when it is
  // dropped.
  std::expected<std::optional<scene::ParameterValue>, std::string> Value(
      const pxr::TfToken& name, const scene::MaterialNode& node,
      const pxr::TfToken& param);
  std::expected<std::optional<scene::ParameterValue>, std::string> Texture(
      const pxr::TfToken& name, const scene::MaterialNode& node,
      const pxr::SdfAssetPath& asset, const pxr::TfToken& color_space);
  // The absolute file of an asset of the node `name`, or none when it does
  // not resolve.
  std::optional<std::string> Resolve(const pxr::TfToken& name,
                                     const pxr::SdfAssetPath& asset);

  const pxr::HdMaterialNetworkInterface* network_;
  std::map<pxr::TfToken, std::size_t> indices_;
  std::vector<UnresolvedTexture> unresolved_;
  std::optional<Error> fatal_;
};

std::expected<TranslatedMaterial, Error> Translator::Translate() {
  const pxr::TfTokenVector names = SurfaceNodes();
  for (std::size_t i = 0; i < names.size(); ++i) {
    indices_.emplace(names.at(i), i);
  }
  TranslatedMaterial translated{
      .network = Network(names),
      .unresolved = std::move(unresolved_),
      .unsupported_terminals = {},
  };
  if (fatal_.has_value()) {
    return std::unexpected(*fatal_);
  }
  for (const pxr::TfToken& terminal : network_->GetTerminalNames()) {
    if (terminal != pxr::HdMaterialTerminalTokens->surface &&
        network_->GetTerminalConnection(terminal).first) {
      translated.unsupported_terminals.push_back(terminal.GetString());
    }
  }
  return translated;
}

pxr::TfTokenVector Translator::SurfaceNodes() const {
  const auto [connected, surface] =
      network_->GetTerminalConnection(pxr::HdMaterialTerminalTokens->surface);
  if (!connected) {
    return {};
  }
  pxr::TfTokenVector nodes = {surface.upstreamNodeName};
  // `nodes` grows as the walk finds the nodes upstream of those it holds.
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const pxr::TfTokenVector upstream = Upstream(nodes.at(i));
    for (const pxr::TfToken& node :
         upstream | std::views::filter([&nodes](const pxr::TfToken& found) {
           return !std::ranges::contains(nodes, found);
         })) {
      nodes.push_back(node);
    }
  }
  return nodes;
}

pxr::TfTokenVector Translator::Upstream(const pxr::TfToken& name) const {
  pxr::TfTokenVector upstream;
  for (const pxr::TfToken& input :
       network_->GetNodeInputConnectionNames(name)) {
    for (const auto& connection :
         network_->GetNodeInputConnection(name, input)) {
      upstream.push_back(connection.upstreamNodeName);
    }
  }
  return upstream;
}

std::expected<scene::MaterialNetwork, std::string> Translator::Network(
    const pxr::TfTokenVector& names) {
  scene::MaterialNetwork network{
      .path = network_->GetMaterialPrimPath().GetString(),
      .nodes = {},
      .surface = {},
  };
  for (const pxr::TfToken& name : names) {
    auto node = Node(name);
    if (!node.has_value()) {
      return std::unexpected(node.error());
    }
    network.nodes.push_back(std::move(*node));
  }
  auto surface = Surface();
  if (!surface.has_value()) {
    return std::unexpected(surface.error());
  }
  network.surface = std::move(*surface);
  return network;
}

std::expected<scene::NodeOutput, std::string> Translator::Surface() {
  const auto [connected, connection] =
      network_->GetTerminalConnection(pxr::HdMaterialTerminalTokens->surface);
  const auto found = indices_.find(connection.upstreamNodeName);
  if (!connected || found == indices_.end()) {
    return std::unexpected("no surface");
  }
  return scene::NodeOutput{
      .node = found->second,
      .name = connection.upstreamOutputName.GetString(),
  };
}

pxr::SdfPath Translator::NodePath(const pxr::TfToken& name) const {
  return network_->GetMaterialPrimPath().AppendPath(
      pxr::SdfPath(name.GetString()));
}

std::expected<scene::MaterialNode, std::string> Translator::Node(
    const pxr::TfToken& name) {
  scene::MaterialNode node{
      .name = name.GetString(),
      .kind = scene::ShaderKind::kMaterialX,
      .shader = {},
      .parameters = {},
      .connections = {},
  };
  if (auto shader = AssignShader(name, node); !shader.has_value()) {
    return std::unexpected(shader.error());
  }
  for (const pxr::TfToken& param :
       network_->GetAuthoredNodeParameterNames(name)) {
    auto value = Value(name, node, param);
    if (!value.has_value()) {
      return std::unexpected(std::format("{}.{}: {}",
                                         NodePath(name).GetString(),
                                         param.GetString(), value.error()));
    }
    if (value->has_value()) {
      node.parameters.push_back({
          .name = param.GetString(),
          .value = std::move(**value),
      });
    }
  }
  auto connections = Connections(name);
  if (!connections.has_value()) {
    return std::unexpected(connections.error());
  }
  node.connections = std::move(*connections);
  return node;
}

std::expected<std::vector<scene::Connection>, std::string>
Translator::Connections(const pxr::TfToken& name) const {
  std::vector<scene::Connection> connections;
  for (const pxr::TfToken& input :
       network_->GetNodeInputConnectionNames(name)) {
    auto connected = Connect(name, input);
    if (!connected.has_value()) {
      return std::unexpected(connected.error());
    }
    connections.append_range(std::move(*connected));
  }
  return connections;
}

std::expected<std::vector<scene::Connection>, std::string> Translator::Connect(
    const pxr::TfToken& name, const pxr::TfToken& input) const {
  std::vector<scene::Connection> connections;
  for (const auto& connection : network_->GetNodeInputConnection(name, input)) {
    const auto found = indices_.find(connection.upstreamNodeName);
    if (found == indices_.end()) {
      return std::unexpected(
          std::format("{}.{} connects to {}, which is not in the network",
                      NodePath(name).GetString(), input.GetString(),
                      connection.upstreamNodeName.GetString()));
    }
    connections.push_back({
        .input = input.GetString(),
        .node = found->second,
        .output = connection.upstreamOutputName.GetString(),
    });
  }
  return connections;
}

std::expected<void, std::string> Translator::AssignShader(
    const pxr::TfToken& name, scene::MaterialNode& node) {
  // A MaterialX nodedef first, then OSL, as ShaderSourceTypes orders them.
  const pxr::TfToken shader_id = network_->GetNodeType(name);
  if (std::optional<std::string> nodedef = NodeDef(shader_id.GetString())) {
    node.kind = scene::ShaderKind::kMaterialX;
    node.shader = std::move(*nodedef);
    return {};
  }
  static const pxr::TfToken kOslSourceAsset("OSL:sourceAsset");
  if (!network_->GetNodeTypeInfoValue(name, kOslSourceAsset).IsEmpty()) {
    return AssignOslShader(name, node);
  }
  return std::unexpected(
      shader_id.IsEmpty()
          ? std::format("{} has no MaterialX nodedef nor OSL shader",
                        NodePath(name).GetString())
          : std::format("{} has the shader {}, which is not a MaterialX "
                        "nodedef",
                        NodePath(name).GetString(), shader_id.GetString()));
}

std::expected<void, std::string> Translator::AssignOslShader(
    const pxr::TfToken& name, scene::MaterialNode& node) {
  static const pxr::TfToken kOslSourceAsset("OSL:sourceAsset");
  const pxr::VtValue asset =
      network_->GetNodeTypeInfoValue(name, kOslSourceAsset);
  if (!asset.IsHolding<pxr::SdfAssetPath>()) {
    return std::unexpected(std::format("{}: OSL:sourceAsset is not an asset",
                                       NodePath(name).GetString()));
  }
  const auto& path = asset.UncheckedGet<pxr::SdfAssetPath>();
  std::optional<std::string> file = Resolve(name, path);
  if (!file.has_value()) {
    return std::unexpected(std::format("{}: the shader {} does not resolve",
                                       NodePath(name).GetString(),
                                       path.GetAssetPath()));
  }
  if (!file->ends_with(".oso")) {
    return std::unexpected(
        std::format("{}: the shader {} is not compiled (.oso)",
                    NodePath(name).GetString(), *file));
  }
  node.kind = scene::ShaderKind::kOsl;
  node.shader = std::move(*file);
  return {};
}

std::optional<std::string> Translator::Resolve(const pxr::TfToken& name,
                                               const pxr::SdfAssetPath& asset) {
  const std::string& resolved = asset.GetResolvedPath();
  if (resolved.empty()) {
    return std::nullopt;
  }
  if (pxr::ArIsPackageRelativePath(resolved) && !fatal_.has_value()) {
    fatal_ = Error(
        ErrorCode::kInvalidArgument,
        std::format("{}: {} is inside the package {}, and umbra reads only "
                    "files on disk",
                    NodePath(name).GetString(), asset.GetAssetPath(),
                    pxr::ArSplitPackageRelativePathOuter(resolved).first));
  }
  return resolved;
}

std::expected<std::optional<scene::ParameterValue>, std::string>
Translator::Value(const pxr::TfToken& name, const scene::MaterialNode& node,
                  const pxr::TfToken& param) {
  const pxr::HdMaterialNetworkInterface::NodeParamData data =
      network_->GetNodeParameterData(name, param);
  const pxr::VtValue& value = data.value;
  if (value.IsEmpty()) {
    return std::nullopt;
  }
  if (value.IsHolding<pxr::SdfAssetPath>()) {
    return Texture(name, node, value.UncheckedGet<pxr::SdfAssetPath>(),
                   data.colorSpace);
  }
  if (IsColorType(data.typeName)) {
    if (std::optional<ColorNumbers> color = ColorComponents(value)) {
      return ConvertedColor(std::move(*color), data.colorSpace);
    }
  }
  std::optional<scene::ParameterValue> plain = PlainValue(value);
  if (!plain.has_value()) {
    return std::unexpected(std::format(
        "a value of type {}, which umbra does not take", value.GetTypeName()));
  }
  return plain;
}

std::expected<std::optional<scene::ParameterValue>, std::string>
Translator::Texture(const pxr::TfToken& name, const scene::MaterialNode& node,
                    const pxr::SdfAssetPath& asset,
                    const pxr::TfToken& color_space) {
  if (asset.GetAssetPath().empty()) {
    return std::nullopt;
  }
  std::optional<std::string> file = Resolve(name, asset);
  if (!file.has_value()) {
    unresolved_.push_back({
        .node = NodePath(name),
        .asset = asset.GetAssetPath(),
    });
    return std::nullopt;
  }
  auto resolved_space = TextureColorSpace(color_space, *file, node.shader);
  if (!resolved_space.has_value()) {
    return std::unexpected(resolved_space.error());
  }
  return scene::Texture{
      .file = std::move(*file),
      .color_space = std::move(*resolved_space),
  };
}

// A MaterialX node named `name` of the nodedef `shader`.
scene::MaterialNode MaterialXNode(std::string name, std::string shader) {
  return {
      .name = std::move(name),
      .kind = scene::ShaderKind::kMaterialX,
      .shader = std::move(shader),
      .parameters = {},
      .connections = {},
  };
}

}  // namespace

std::expected<TranslatedMaterial, Error> TranslateMaterial(
    const pxr::HdMaterialNetworkInterface& network) {
  return Translator(network).Translate();
}

MaterialPrim::MaterialPrim(const pxr::SdfPath& prim_id,
                           core::ProgressPhase* UMBRA_NULLABLE phase)
    : pxr::HdMaterial(prim_id),
      phase_(phase),
      translation_(TranslatedMaterial{
          .network = std::unexpected("not synced"),
          .unresolved = {},
          .unsupported_terminals = {},
      }) {}

pxr::HdDirtyBits MaterialPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdMaterial::AllDirty;
}

void MaterialPrim::Sync(pxr::HdSceneDelegate* scene_delegate,
                        pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                        pxr::HdDirtyBits* dirty_bits) {
  *dirty_bits = pxr::HdMaterial::Clean;
  if (phase_ == nullptr) {
    return;
  }
  const pxr::HdSceneIndexBaseRefPtr terminal =
      scene_delegate->GetRenderIndex().GetTerminalSceneIndex();
  const pxr::HdContainerDataSourceHandle prim =
      terminal != nullptr ? terminal->GetPrim(GetId()).dataSource : nullptr;
  const pxr::HdMaterialNetworkSchema schema =
      pxr::HdMaterialSchema::GetFromParent(prim).GetMaterialNetwork(
          MaterialRenderContexts());
  if (schema.IsDefined()) {
    const pxr::HdDataSourceMaterialNetworkInterface network(
        GetId(), schema.GetContainer(), prim);
    translation_ = TranslateMaterial(network);
  } else {
    translation_ = TranslatedMaterial{
        .network = std::unexpected("no MaterialX or universal surface"),
        .unresolved = {},
        .unsupported_terminals = {},
    };
  }
  phase_->Add(1);
  if (const scene::MaterialNetwork* synced = network(); synced != nullptr) {
    const std::size_t nodes = synced->nodes.size();
    UMBRA_LOG_TRACE(kUsd, "synced {} ({} node{})", GetId().GetString(), nodes,
                    nodes == 1 ? "" : "s");
  } else {
    UMBRA_LOG_TRACE(kUsd, "synced {}", GetId().GetString());
  }
}

const scene::MaterialNetwork* UMBRA_NULLABLE MaterialPrim::network() const {
  if (!translation_.has_value() || !translation_->network.has_value()) {
    return nullptr;
  }
  return &*translation_->network;
}

const pxr::TfTokenVector& MaterialRenderContexts() {
  static const pxr::TfTokenVector kContexts = {
      pxr::TfToken("mtlx"),
      pxr::HdMaterialSchemaTokens->universalRenderContext,
  };
  return kContexts;
}

const pxr::TfTokenVector& ShaderSourceTypes() {
  static const pxr::TfTokenVector kTypes = {
      pxr::TfToken("mtlx"),
      pxr::TfToken("OSL"),
  };
  return kTypes;
}

scene::MaterialNetwork FallbackMaterial() {
  scene::MaterialNode bsdf =
      MaterialXNode("diffuse", "ND_oren_nayar_diffuse_bsdf");
  bsdf.parameters.push_back({
      .name = "color",
      .value = scene::Color{.rgb = {kGray, kGray, kGray}},
  });
  scene::MaterialNode surface = MaterialXNode("surface", "ND_surface");
  surface.connections.push_back({.input = "bsdf", .node = 0, .output = "out"});
  return {
      .path = std::string(kFallbackMaterial),
      .nodes = {std::move(bsdf), std::move(surface)},
      .surface = {.node = 1, .name = "out"},
  };
}

scene::MaterialNetwork DisplayColorMaterial() {
  scene::MaterialNode reader =
      MaterialXNode("displayColor", "ND_UsdPrimvarReader_vector3");
  reader.parameters.push_back(
      {.name = "varname", .value = std::string(kDisplayColor)});
  reader.parameters.push_back({
      .name = "fallback",
      .value = std::array<float, 3>{kGray, kGray, kGray},
  });
  // The reader gives a vector3, which MaterialX does not connect to a color.
  scene::MaterialNode color =
      MaterialXNode("color", "ND_convert_vector3_color3");
  color.connections.push_back({.input = "in", .node = 0, .output = "out"});
  scene::MaterialNode surface =
      MaterialXNode("surface", "ND_UsdPreviewSurface_surfaceshader");
  surface.connections.push_back(
      {.input = "diffuseColor", .node = 1, .output = "out"});
  return {
      .path = std::string(kDisplayColorMaterial),
      .nodes = {std::move(reader), std::move(color), std::move(surface)},
      .surface = {.node = 2, .name = "out"},
  };
}

void ConvertDisplayColor(pxr::HdSceneDelegate& delegate,
                         const pxr::SdfPath& prim_id,
                         std::vector<scene::Primvar>& primvars) {
  const auto display =
      std::ranges::find_if(primvars, [](const scene::Primvar& primvar) {
        return primvar.name == kDisplayColor && primvar.components == 3 &&
               std::holds_alternative<std::vector<float>>(primvar.values);
      });
  if (display == primvars.end()) {
    return;
  }
  static const pxr::TfToken kName{std::string(kDisplayColor)};
  const pxr::HdSceneIndexBaseRefPtr terminal =
      delegate.GetRenderIndex().GetTerminalSceneIndex();
  const pxr::HdTokenDataSourceHandle authored =
      terminal == nullptr ? nullptr
                          : pxr::HdPrimvarsSchema::GetFromParent(
                                terminal->GetPrim(prim_id).dataSource)
                                .GetPrimvar(kName)
                                .GetColorSpace();
  const pxr::TfToken color_space =
      authored != nullptr ? authored->GetTypedValue(0.0F) : pxr::TfToken();
  auto converted = ocio::ConvertToAcescg(
      color_space.IsEmpty() ? kDefaultColorSpace : color_space.GetString(),
      std::get<std::vector<float>>(display->values));
  if (!converted.has_value()) {
    UMBRA_LOG_WARNING(kUsd, "{}: displayColor dropped: {}", prim_id.GetString(),
                      converted.error().message());
    primvars.erase(display);
  }
}

}  // namespace umbra::ext::usd::internal
