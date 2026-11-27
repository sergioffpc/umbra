// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "tev.h"

#include <netdb.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "channels.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/delegate/session.h"
#include "umbra/device/image.h"

namespace umbra {
namespace {

using core::Error;
using core::ErrorCode;

// tev reads the integers and floats of a packet little-endian.
static_assert(std::endian::native == std::endian::little);

// The port of `text`, from 1 to 65535.
std::expected<std::uint16_t, Error> ParsePort(std::string_view text) {
  std::uint32_t port = 0;
  const char* const first = std::to_address(text.begin());
  const char* const last = std::to_address(text.end());
  const auto [end, error] = std::from_chars(first, last, port);
  if (error != std::errc() || end != last || port == 0 || port > 65535) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("invalid port '{}' for --display: a number from 1 to "
                          "65535",
                          text)));
  }
  return static_cast<std::uint16_t>(port);
}

// Waits until `descriptor` can be written to, for at most kTevTimeoutMs: 0,
// or the errno of why it cannot.
int AwaitWritable(int descriptor) {
  pollfd writable{.fd = descriptor, .events = POLLOUT, .revents = 0};
  const int ready = poll(&writable, 1, kTevTimeoutMs);
  if (ready == 0) {
    return ETIMEDOUT;
  }
  return ready < 0 ? errno : 0;
}

// Sends what `descriptor` takes of `bytes` once it has room, within
// kTevTimeoutMs: how many bytes it took, or the errno of why it cannot.
std::expected<std::size_t, int> SendSome(int descriptor,
                                         std::span<const std::byte> bytes) {
  if (const int waited = AwaitWritable(descriptor); waited != 0) {
    return std::unexpected(waited);
  }
  const auto sent = send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL);
  if (sent > 0) {
    return static_cast<std::size_t>(sent);
  }
  if (sent == 0) {
    return std::unexpected(ECONNRESET);
  }
  if (errno == EAGAIN || errno == EINTR) {
    return 0;
  }
  return std::unexpected(errno);
}

// Connects `descriptor`, a nonblocking socket, to `info`'s address within
// kTevTimeoutMs: 0, or the errno of the failure.
int ConnectWithin(int descriptor, const addrinfo& info) {
  if (connect(descriptor, info.ai_addr, info.ai_addrlen) == 0) {
    return 0;
  }
  if (errno != EINPROGRESS) {
    return errno;
  }
  if (const int waited = AwaitWritable(descriptor); waited != 0) {
    return waited;
  }
  // Connecting again gives the outcome: EISCONN once connected.
  if (connect(descriptor, info.ai_addr, info.ai_addrlen) == 0 ||
      errno == EISCONN) {
    return 0;
  }
  return errno;
}

// A nonblocking socket of `info`'s, connected to its address, or the errno
// of the failure.
std::expected<int, int> ConnectTo(const addrinfo& info) {
  const int descriptor =
      socket(info.ai_family,
             static_cast<int>(static_cast<unsigned int>(info.ai_socktype) |
                              static_cast<unsigned int>(SOCK_NONBLOCK)),
             info.ai_protocol);
  if (descriptor < 0) {
    return std::unexpected(errno);
  }
  if (const int error = ConnectWithin(descriptor, info); error != 0) {
    close(descriptor);
    return std::unexpected(error);
  }
  return descriptor;
}

// A nonblocking socket connected to `address`, or why there is none.
std::expected<int, Error> Connect(const TevAddress& address) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const std::string port = std::to_string(address.port);
  if (const int status =
          getaddrinfo(address.host.c_str(), port.c_str(), &hints, &found);
      status != 0) {
    return std::unexpected(
        Error(ErrorCode::kUnavailable, gai_strerror(status)));
  }
  std::expected<int, Error> connected = std::unexpected(
      Error(ErrorCode::kUnavailable, "the host has no address"));
  for (const addrinfo* info = found; info != nullptr; info = info->ai_next) {
    const std::expected<int, int> socket = ConnectTo(*info);
    if (socket.has_value()) {
      connected = *socket;
      break;
    }
    connected =
        std::unexpected(Error(ErrorCode::kUnavailable,
                              std::generic_category().message(socket.error())));
  }
  freeaddrinfo(found);
  return connected;
}

// The channels of `var` that have a name, and are shown: its first ones.
std::size_t ShownChannels(const delegate::RenderedVar& var) {
  return ChannelNames(var.var->source, var.var->channels).size();
}

}  // namespace

std::expected<TevAddress, Error> ParseDisplay(std::string_view text) {
  constexpr std::string_view kTev = "tev";
  if (!text.starts_with(kTev) ||
      (text.size() > kTev.size() && text.at(kTev.size()) != ':')) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("unknown display '{}': the display is "
                          "tev[:<host>[:<port>]]",
                          text)));
  }
  TevAddress address;
  if (text.size() <= kTev.size()) {
    return address;
  }
  const std::string_view rest = text.substr(kTev.size() + 1);
  const std::size_t colon = rest.rfind(':');
  const std::string_view host = rest.substr(0, colon);
  if (!host.empty()) {
    address.host = host;
  }
  if (colon != std::string_view::npos) {
    const auto port = ParsePort(rest.substr(colon + 1));
    if (!port.has_value()) {
      return std::unexpected(port.error());
    }
    address.port = *port;
  }
  return address;
}

void Packet::Start(PacketType type) {
  bytes_.clear();
  Put(std::uint32_t{0});
  Put(type);
}

void Packet::PutFloats(std::span<const float> values) {
  const auto bytes = std::as_bytes(values);
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void Packet::PutHalves(std::span<const float> values) {
  for (const float value : values) {
    // The conversion rounds to the nearest half, ties to even, as OpenEXR.
    Put(static_cast<float>(static_cast<_Float16>(value)));
  }
}

void Packet::PutString(std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text));
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  bytes_.push_back(std::byte{0});
}

std::span<const std::byte> Packet::Seal() {
  const auto size = static_cast<std::uint32_t>(bytes_.size());
  std::ranges::copy(std::as_bytes(std::span(&size, 1)), bytes_.begin());
  return bytes_;
}

TevDisplay::TevDisplay(TevAddress address) : address_(std::move(address)) {}

TevDisplay::~TevDisplay() { Close(); }

void TevDisplay::ConnectOnce() {
  if (connection_ != Connection::kNotTried) {
    return;
  }
  const auto connected = Connect(address_);
  if (!connected.has_value()) {
    connection_ = Connection::kClosed;
    UMBRA_LOG_WARNING(
        kCore, "cannot reach tev at {}:{} ({}); rendering without it",
        address_.host, address_.port, connected.error().message());
    return;
  }
  socket_ = *connected;
  connection_ = Connection::kOpen;
}

void TevDisplay::Send() {
  std::span<const std::byte> left = packet_.Seal();
  while (!left.empty()) {
    const std::expected<std::size_t, int> sent = SendSome(socket_, left);
    if (!sent.has_value()) {
      UMBRA_LOG_WARNING(kCore, "lost tev at {}:{} ({}); rendering without it",
                        address_.host, address_.port,
                        std::generic_category().message(sent.error()));
      Close();
      return;
    }
    left = left.subspan(*sent);
  }
}

void TevDisplay::Close() {
  if (socket_ >= 0) {
    close(socket_);
    socket_ = -1;
  }
  connection_ = Connection::kClosed;
}

void TevDisplay::Begin(const delegate::RenderProduct& product,
                       std::span<const delegate::RenderedVar> vars) {
  product_ = &product;
  vars_ = vars;
  channels_.clear();
  for (const delegate::RenderedVar& var : vars) {
    for (const std::string_view channel :
         ChannelNames(var.var->source, var.var->channels)) {
      channels_.push_back(std::format("{}.{}", var.var->name, channel));
    }
  }
  ConnectOnce();
  if (connection_ != Connection::kOpen) {
    return;
  }
  packet_.Start(PacketType::kCreateImage);
  packet_.Put(std::uint8_t{0});  // Grab focus: no.
  packet_.PutString(product.file);
  packet_.Put(static_cast<std::int32_t>(product.window.width()));
  packet_.Put(static_cast<std::int32_t>(product.window.height()));
  packet_.Put(static_cast<std::int32_t>(channels_.size()));
  for (const std::string& channel : channels_) {
    packet_.PutString(channel);
  }
  Send();
}

void TevDisplay::Update(const device::PixelWindow& region) {
  if (product_ == nullptr || connection_ != Connection::kOpen ||
      channels_.empty()) {
    return;
  }
  packet_.Start(PacketType::kUpdateImageV3);
  packet_.Put(std::uint8_t{0});  // Grab focus: no.
  packet_.PutString(product_->file);
  packet_.Put(static_cast<std::int32_t>(channels_.size()));
  for (const std::string& channel : channels_) {
    packet_.PutString(channel);
  }
  const device::PixelWindow& window = product_->window;
  packet_.Put(static_cast<std::int32_t>(region.x_min - window.x_min));
  packet_.Put(static_cast<std::int32_t>(region.y_min - window.y_min));
  packet_.Put(static_cast<std::int32_t>(region.width()));
  packet_.Put(static_cast<std::int32_t>(region.height()));
  PutLayout(region);
  for (const delegate::RenderedVar& var : vars_) {
    PutPixels(var, region);
  }
  Send();
}

void TevDisplay::PutLayout(const device::PixelWindow& region) {
  // Each var's pixels as its render buffer holds them, its channels
  // interleaved, row by row, after the last var's.
  const auto count = static_cast<std::size_t>(region.width()) *
                     static_cast<std::size_t>(region.height());
  std::size_t first_value = 0;
  for (const delegate::RenderedVar& var : vars_) {
    for (std::size_t channel = 0; channel < ShownChannels(var); ++channel) {
      packet_.Put(static_cast<std::int64_t>(first_value + channel));
    }
    first_value += count * var.var->channels;
  }
  for (const delegate::RenderedVar& var : vars_) {
    for (std::size_t channel = 0; channel < ShownChannels(var); ++channel) {
      packet_.Put(static_cast<std::int64_t>(var.var->channels));
    }
  }
}

void TevDisplay::PutPixels(const delegate::RenderedVar& var,
                           const device::PixelWindow& region) {
  const device::PixelWindow& window = product_->window;
  const auto window_width = static_cast<std::size_t>(window.width());
  const std::size_t channels = var.var->channels;
  const std::size_t values =
      static_cast<std::size_t>(region.width()) * channels;
  const bool is_half = var.var->type == delegate::ChannelType::kHalf;
  for (std::int32_t row = region.y_min; row < region.y_end; ++row) {
    const std::size_t first =
        (static_cast<std::size_t>(row - window.y_min) * window_width) +
        static_cast<std::size_t>(region.x_min - window.x_min);
    const std::span<const float> row_values =
        var.pixels.subspan(first * channels, values);
    if (is_half) {
      packet_.PutHalves(row_values);
    } else {
      packet_.PutFloats(row_values);
    }
  }
}

void TevDisplay::End() {
  product_ = nullptr;
  vars_ = {};
}

}  // namespace umbra
