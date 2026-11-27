// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The display that shows the products in tev, the HDR image viewer, while
// they render: one image per product per frame, named after its file and
// sized to its data window, with the channels of its OpenEXR file, sent over
// tev's TCP protocol as its tiles finish.

#ifndef UMBRA_APPS_UMBRA_TEV_H_
#define UMBRA_APPS_UMBRA_TEV_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/image.h"
#include "umbra/ext/usd/render_settings.h"
#include "umbra/ext/usd/usd.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra {

// The longest a display waits on tev, to connect or for room to send.
inline constexpr int kTevTimeoutMs = 1000;

// Where tev listens.
struct TevAddress {
  std::string host = "127.0.0.1";
  std::uint16_t port = 14158;
};

// The address of --display's `text`, "tev[:<host>[:<port>]]". Fails with
// kInvalidArgument for another display, or a port that is not a number from
// 1 to 65535.
[[nodiscard]] std::expected<TevAddress, core::Error> ParseDisplay(
    std::string_view text);

// The types of tev's packets that umbra sends.
enum class PacketType : std::uint8_t {
  kCreateImage = 4,
  kUpdateImageV3 = 6,
};

// A packet of tev's protocol, built in place: its size, its own included,
// then its type and its fields, little-endian, each string followed by a
// NUL.
class Packet {
 public:
  // Starts a packet of `type`, dropping the one built.
  void Start(PacketType type);
  // Appends `value`'s bytes.
  template <typename T>
  void Put(const T& value) {
    const auto bytes = std::as_bytes(std::span(&value, 1));
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }
  // Appends the bytes of `values`.
  void PutFloats(std::span<const float> values);
  // Appends the bytes of `values`, each rounded to the nearest half, as an
  // OpenEXR file of half channels holds it.
  void PutHalves(std::span<const float> values);
  // Appends `text` and a NUL.
  void PutString(std::string_view text);
  // Writes its size, and gives its bytes, valid until the next Start.
  [[nodiscard]] std::span<const std::byte> Seal() UMBRA_LIFETIMEBOUND;

 private:
  std::vector<std::byte> bytes_;
};

// Shows the products in the tev at an address. It connects as the first
// product begins; when it cannot, or the connection drops or stalls for
// kTevTimeoutMs, it warns once and shows nothing more. It never waits longer
// than kTevTimeoutMs on tev.
class TevDisplay final : public ext::usd::Display {
 public:
  explicit TevDisplay(TevAddress address);
  TevDisplay(const TevDisplay&) = delete;
  TevDisplay& operator=(const TevDisplay&) = delete;
  TevDisplay(TevDisplay&&) = delete;
  TevDisplay& operator=(TevDisplay&&) = delete;
  ~TevDisplay() override;

  void Begin(const ext::usd::RenderProduct& product,
             std::span<const ext::usd::RenderedVar> vars) override;
  void Update(const device::PixelWindow& region) override;
  void End() override;

 private:
  // Whether it connected, has yet to, or gave up.
  enum class Connection : std::uint8_t { kNotTried, kOpen, kClosed };

  // Connects, the first time it is called: warns when it cannot.
  void ConnectOnce();
  // Sends the packet built; when it cannot, warns and closes.
  void Send();
  // Closes the connection, for good.
  void Close();
  // Appends to the packet where each channel's values of `region` are.
  void PutLayout(const device::PixelWindow& region);
  // Appends to the packet the values of `var` over `region`.
  void PutPixels(const ext::usd::RenderedVar& var,
                 const device::PixelWindow& region);

  TevAddress address_;
  Connection connection_ = Connection::kNotTried;
  // The socket, while the connection is open.
  int socket_ = -1;
  // The product shown, its vars and the names of their channels.
  const ext::usd::RenderProduct* UMBRA_NULLABLE product_ = nullptr;
  std::span<const ext::usd::RenderedVar> vars_;
  std::vector<std::string> channels_;
  // The packet being built.
  Packet packet_;
};

}  // namespace umbra

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_APPS_UMBRA_TEV_H_
