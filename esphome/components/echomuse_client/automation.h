#pragma once

#include "echomuse_client.h"
#include "esphome/core/automation.h"

namespace esphome {
namespace echomuse_client {

class OnLedsTrigger : public Trigger<std::vector<uint8_t>> {
 public:
  explicit OnLedsTrigger(EchoMuseClient *parent) { parent->add_leds_trigger(this); }
};

class OnVolumeSetTrigger : public Trigger<float> {
 public:
  explicit OnVolumeSetTrigger(EchoMuseClient *parent) { parent->add_volume_trigger(this); }
};

class OnPhaseTrigger : public Trigger<std::string> {
 public:
  explicit OnPhaseTrigger(EchoMuseClient *parent) { parent->add_phase_trigger(this); }
};

}  // namespace echomuse_client
}  // namespace esphome
