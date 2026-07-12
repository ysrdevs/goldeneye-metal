#include "ui/macos_key_translation.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using rex::ui::VirtualKey;
using rex::ui::macos::TranslateKeyCode;

TEST_CASE("macOS key codes map to common virtual keys", "[ui][macos][input]") {
  CHECK(TranslateKeyCode(0x00) == VirtualKey::kA);
  CHECK(TranslateKeyCode(0x0D) == VirtualKey::kW);
  CHECK(TranslateKeyCode(0x31) == VirtualKey::kSpace);
  CHECK(TranslateKeyCode(0x35) == VirtualKey::kEscape);
  CHECK(TranslateKeyCode(0x38) == VirtualKey::kShift);
  CHECK(TranslateKeyCode(0x3E) == VirtualKey::kControl);
  CHECK(TranslateKeyCode(0x7A) == VirtualKey::kF1);
  CHECK(TranslateKeyCode(0x7B) == VirtualKey::kLeft);
  CHECK(TranslateKeyCode(0x7E) == VirtualKey::kUp);
  CHECK(TranslateKeyCode(0x53) == VirtualKey::kNumpad1);
}

TEST_CASE("unknown macOS key codes are ignored", "[ui][macos][input]") {
  CHECK(TranslateKeyCode(0xFFFF) == VirtualKey::kNone);
}

}  // namespace
