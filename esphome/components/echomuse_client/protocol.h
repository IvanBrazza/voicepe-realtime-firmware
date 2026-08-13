#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome::echomuse_client::protocol {

static constexpr size_t MIC_SAMPLES_PER_PACKET = 1280;
static constexpr size_t MIC_BYTES_PER_PACKET = MIC_SAMPLES_PER_PACKET * 2;

inline int16_t stereo_s32_to_mono_s16(const uint8_t *frame, uint8_t channel) {
  int32_t sample = 0;
  std::memcpy(&sample, frame + (channel & 1U) * sizeof(int32_t), sizeof(sample));
  return static_cast<int16_t>(sample >> 16);
}

inline void write_mic_header(uint8_t *packet, uint16_t sequence) {
  packet[0] = 0x01;
  packet[1] = static_cast<uint8_t>(sequence >> 8);
  packet[2] = static_cast<uint8_t>(sequence);
}

inline uint16_t next_sequence(uint16_t sequence) {
  return static_cast<uint16_t>(sequence + 1U);
}

inline float level_to_pe_volume(int level) {
  level = std::max(0, std::min(175, level));
  return level == 0 ? 0.0f : 0.40f + ((level - 1) / 174.0f) * 0.52f;
}

inline int pe_volume_to_level(float volume, bool muted) {
  if (muted || volume <= 0.0f)
    return 0;
  const float normal = (std::max(0.40f, std::min(0.92f, volume)) - 0.40f) / 0.52f;
  return std::max(1, std::min(175,
      1 + static_cast<int>(std::lround(normal * 174.0f))));
}

class FragmentAccumulator {
 public:
  bool append(size_t offset, size_t total, const uint8_t *data, size_t len) {
    if (offset == 0)
      bytes_.clear();
    if (offset != bytes_.size() || data == nullptr)
      return false;
    bytes_.insert(bytes_.end(), data, data + len);
    return offset + len >= total;
  }
  const std::vector<uint8_t> &bytes() const { return bytes_; }
  void clear() { bytes_.clear(); }

 protected:
  std::vector<uint8_t> bytes_;
};

inline bool ring_write(uint8_t *buffer, size_t capacity, size_t &tail,
                       size_t &fill, const uint8_t *data, size_t len) {
  if (buffer == nullptr || data == nullptr || len > capacity - fill)
    return false;
  const size_t first = std::min(len, capacity - tail);
  std::memcpy(buffer + tail, data, first);
  if (len > first)
    std::memcpy(buffer, data + first, len - first);
  tail = (tail + len) % capacity;
  fill += len;
  return true;
}

class PlaybackDiscardGate {
 public:
  void flush() { discarding_ = true; }
  bool accept(uint8_t frame_type) {
    if (frame_type == 0x03) {
      discarding_ = false;
      return false;
    }
    return frame_type == 0x02 && !discarding_;
  }
  bool discarding() const { return discarding_; }

 protected:
  bool discarding_{false};
};

}  // namespace esphome::echomuse_client::protocol
