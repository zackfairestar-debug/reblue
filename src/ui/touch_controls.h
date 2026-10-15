/**
 * @file    ui/touch_controls.h
 * @brief   On-screen gamepad for touch screens (the Android build).
 *
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Drives an SDL virtual gamepad from the fingers on screen, so the game and
 * the SDK input driver see an ordinary controller. Drawn over the frame through
 * the ImGui overlay and hidden while a physical controller is connected.
 */
#pragma once

#include <functional>
#include <unordered_map>

#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_touch.h>

#include <rex/ui/imgui_dialog.h>

struct ImGuiIO;

namespace rex::ui {
class ImGuiDrawer;
} // namespace rex::ui

namespace bd::ui {

class TouchControls final : public rex::ui::ImGuiDialog {
public:
  // `enabled` gates the controls: while it returns false (the installer, an
  // upgrade prompt) touches stay plain mouse input for the ImGui windows.
  TouchControls(rex::ui::ImGuiDrawer *drawer, std::function<bool()> enabled);
  ~TouchControls();

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  // What a finger took hold of when it went down. Buttons are re-hit-tested
  // every frame so a thumb can roll from one to the next; sticks stay with the
  // finger that grabbed them.
  enum class Owner { kButtons, kMoveStick, kLookStick };
  struct Finger {
    Owner owner = Owner::kButtons;
    float start_x = 0, start_y = 0; // pixels
    float x = 0, y = 0;
    bool seen = false;
  };

  bool EnsureVirtualPad();
  bool PhysicalPadConnected() const;
  void ReleaseAll();
  void SetMouseFromTouch(bool enabled);

  std::function<bool()> enabled_;
  SDL_JoystickID pad_id_ = 0;
  SDL_Joystick *pad_ = nullptr;
  std::unordered_map<SDL_FingerID, Finger> fingers_;
  bool visible_ = false;
  int mouse_from_touch_ = -1; // last value written to the SDL hint
};

} // namespace bd::ui
