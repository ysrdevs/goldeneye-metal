#include <rex/ui/ui_event.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Mouse events distinguish absolute and relative motion", "[ui][input][mouse]") {
  rex::ui::MouseEvent absolute(nullptr, rex::ui::MouseEvent::Button::kNone, 12, 34);
  CHECK_FALSE(absolute.has_movement_delta());
  CHECK(absolute.movement_x() == 0);
  CHECK(absolute.movement_y() == 0);

  rex::ui::MouseEvent relative(nullptr, rex::ui::MouseEvent::Button::kNone, 12, 34, 0, 0, {-5, 7});
  CHECK(relative.has_movement_delta());
  CHECK(relative.movement_x() == -5);
  CHECK(relative.movement_y() == 7);
  CHECK(relative.x() == 12);
  CHECK(relative.y() == 34);
}
