#include "../esphome/components/echomuse_client/protocol.h"

#include <array>
#include <cassert>
#include <cmath>
#include <string>

using namespace esphome::echomuse_client::protocol;

int main() {
  const std::array<int32_t, 2> stereo{0x12340000, -0x23450000};
  const auto *frame = reinterpret_cast<const uint8_t *>(stereo.data());
  assert(stereo_s32_to_mono_s16(frame, 0) == 0x1234);
  assert(stereo_s32_to_mono_s16(frame, 1) == static_cast<int16_t>(-0x2345));

  std::array<uint8_t, 3> header{};
  write_mic_header(header.data(), 0xFFFF);
  assert((header == std::array<uint8_t, 3>{0x01, 0xFF, 0xFF}));
  assert(next_sequence(0xFFFF) == 0);

  assert(level_to_pe_volume(0) == 0.0f);
  assert(std::fabs(level_to_pe_volume(1) - 0.40f) < 0.0001f);
  assert(std::fabs(level_to_pe_volume(175) - 0.92f) < 0.0001f);
  assert(pe_volume_to_level(0.40f, false) == 1);
  assert(pe_volume_to_level(0.92f, false) == 175);
  assert(pe_volume_to_level(0.75f, true) == 0);

  FragmentAccumulator fragments;
  const std::string payload = "{\"type\":\"ping\"}";
  assert(!fragments.append(0, payload.size(),
                           reinterpret_cast<const uint8_t *>(payload.data()), 5));
  assert(fragments.append(5, payload.size(),
                          reinterpret_cast<const uint8_t *>(payload.data() + 5),
                          payload.size() - 5));
  assert(std::string(fragments.bytes().begin(), fragments.bytes().end()) == payload);

  std::array<uint8_t, 8> ring{};
  size_t tail = 6, fill = 0;
  const std::array<uint8_t, 4> audio{1, 2, 3, 4};
  assert(ring_write(ring.data(), ring.size(), tail, fill, audio.data(), audio.size()));
  assert(tail == 2 && fill == 4);
  assert(ring[6] == 1 && ring[7] == 2 && ring[0] == 3 && ring[1] == 4);

  PlaybackDiscardGate gate;
  gate.flush();
  assert(!gate.accept(0x02));
  assert(gate.discarding());
  assert(!gate.accept(0x03));
  assert(!gate.discarding());
  assert(gate.accept(0x02));
}
