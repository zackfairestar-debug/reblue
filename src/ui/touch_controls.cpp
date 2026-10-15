/**
 * @file    ui/touch_controls.cpp
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "ui/touch_controls.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <imgui.h>

#include "core/logging.h"

namespace bd::ui {

namespace {

// Reported as a wired Xbox 360 pad, so the HUD shows the A/B/X/Y glyphs the
// on-screen buttons are labelled with.
constexpr uint16_t kXboxVendor = 0x045E;
constexpr uint16_t kXbox360Product = 0x028E;

constexpr ImU32 kFill = IM_COL32(20, 20, 30, 90);
constexpr ImU32 kFillPressed = IM_COL32(220, 220, 240, 140);
constexpr ImU32 kEdge = IM_COL32(255, 255, 255, 120);
constexpr ImU32 kLabel = IM_COL32(255, 255, 255, 200);

struct Circle {
  float x, y, r;
  bool Hit(float px, float py, float slack) const {
    const float dx = px - x, dy = py - y, rr = r * slack;
    return dx * dx + dy * dy <= rr * rr;
  }
};

struct Rect {
  float x0, y0, x1, y1;
  bool Hit(float px, float py, float slack) const {
    return px >= x0 - slack && px <= x1 + slack && py >= y0 - slack &&
           py <= y1 + slack;
  }
  float Cx() const { return (x0 + x1) * 0.5f; }
  float Cy() const { return (y0 + y1) * 0.5f; }
};

struct FaceButton {
  SDL_GamepadButton button;
  const char *label;
  ImU32 color;
};
constexpr FaceButton kFace[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, "A", IM_COL32(96, 192, 64, 255)},
    {SDL_GAMEPAD_BUTTON_EAST, "B", IM_COL32(220, 64, 56, 255)},
    {SDL_GAMEPAD_BUTTON_WEST, "X", IM_COL32(64, 128, 232, 255)},
    {SDL_GAMEPAD_BUTTON_NORTH, "Y", IM_COL32(240, 196, 48, 255)},
};

// Everything is sized in units of the display height, so the controls keep
// their physical size across aspect ratios.
struct Layout {
  float u = 0;
  Circle face[4]{};
  Rect lt{}, lb{}, rb{}, rt{}, back{}, start{};
  float dpad_x = 0, dpad_y = 0, dpad_arm = 0;
  Circle move_rest{};
  float move_radius = 0;
  float look_radius = 0;
  float w = 0, h = 0;

  Layout(float width, float height) : u(height), w(width), h(height) {
    const float cx = w - 0.20f * u, cy = h - 0.27f * u, s = 0.10f * u;
    const float r = 0.058f * u;
    face[0] = {cx, cy + s, r};
    face[1] = {cx + s, cy, r};
    face[2] = {cx - s, cy, r};
    face[3] = {cx, cy - s, r};

    const float top = 0.03f * u, bottom = 0.11f * u, bw = 0.15f * u;
    const float edge = 0.03f * u, gap = 0.02f * u;
    lt = {edge, top, edge + bw, bottom};
    lb = {edge + bw + gap, top, edge + 2 * bw + gap, bottom};
    rt = {w - edge - bw, top, w - edge, bottom};
    rb = {w - edge - 2 * bw - gap, top, w - edge - bw - gap, bottom};
    back = {w * 0.5f - 0.14f * u, top, w * 0.5f - 0.02f * u, 0.095f * u};
    start = {w * 0.5f + 0.02f * u, top, w * 0.5f + 0.14f * u, 0.095f * u};

    dpad_x = 0.20f * u;
    dpad_y = 0.40f * u;
    dpad_arm = 0.075f * u;

    move_rest = {0.27f * u, h - 0.27f * u, 0.0f};
    move_radius = 0.13f * u;
    look_radius = 0.11f * u;
  }

  bool InDpad(float px, float py) const {
    const float dx = px - dpad_x, dy = py - dpad_y;
    const float reach = dpad_arm * 1.9f;
    return dx * dx + dy * dy <= reach * reach;
  }
  bool InMoveZone(float px, float py) const {
    return px < w * 0.45f && py > h * 0.30f;
  }
};

Sint16 ToAxis(float v) {
  return static_cast<Sint16>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
}

void ClampToUnit(float &x, float &y) {
  const float len = std::sqrt(x * x + y * y);
  if (len > 1.0f) {
    x /= len;
    y /= len;
  }
}

void DrawLabel(ImDrawList *dl, float size, float cx, float cy,
               const char *text) {
  ImFont *font = ImGui::GetFont();
  const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size, ImVec2(cx - extent.x * 0.5f, cy - extent.y * 0.5f),
              kLabel, text);
}

} // namespace

TouchControls::TouchControls(rex::ui::ImGuiDrawer *drawer,
                             std::function<bool()> enabled)
    : rex::ui::ImGuiDialog(drawer), enabled_(std::move(enabled)) {}

TouchControls::~TouchControls() {
  if (pad_) {
    SDL_CloseJoystick(pad_);
    SDL_DetachVirtualJoystick(pad_id_);
  }
  SetMouseFromTouch(true);
}

bool TouchControls::EnsureVirtualPad() {
  if (pad_)
    return true;
  // The SDK input driver brings the gamepad subsystem up once the window
  // exists; attaching earlier would leave the pad unannounced.
  if (!SDL_WasInit(SDL_INIT_GAMEPAD))
    return false;

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.vendor_id = kXboxVendor;
  desc.product_id = kXbox360Product;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  // With the masks set SDL derives a gamepad mapping for the device itself.
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) |
                     (1u << SDL_GAMEPAD_BUTTON_EAST) |
                     (1u << SDL_GAMEPAD_BUTTON_WEST) |
                     (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                     (1u << SDL_GAMEPAD_BUTTON_BACK) |
                     (1u << SDL_GAMEPAD_BUTTON_START) |
                     (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) |
                     (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
                     (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
                     (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) |
                     (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) |
                     (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
                     (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
                     (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  desc.name = "re:Blue touch controls";

  pad_id_ = SDL_AttachVirtualJoystick(&desc);
  if (!pad_id_) {
    BD_WARN("Touch controls: SDL_AttachVirtualJoystick failed: {}",
            SDL_GetError());
    // Keep the overlay off rather than retrying every frame.
    enabled_ = [] { return false; };
    return false;
  }
  pad_ = SDL_OpenJoystick(pad_id_);
  if (!pad_) {
    BD_WARN("Touch controls: SDL_OpenJoystick failed: {}", SDL_GetError());
    SDL_DetachVirtualJoystick(pad_id_);
    pad_id_ = 0;
    enabled_ = [] { return false; };
    return false;
  }
  BD_INFO("Touch controls attached as virtual gamepad {}", pad_id_);
  return true;
}

bool TouchControls::PhysicalPadConnected() const {
  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  bool physical = false;
  for (int i = 0; ids && i < count; ++i) {
    if (ids[i] != pad_id_) {
      physical = true;
      break;
    }
  }
  SDL_free(ids);
  return physical;
}

void TouchControls::ReleaseAll() {
  fingers_.clear();
  if (!pad_)
    return;
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    SDL_SetJoystickVirtualButton(pad_, b, false);
  for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
    SDL_SetJoystickVirtualAxis(pad_, a, 0);
}

void TouchControls::SetMouseFromTouch(bool enabled) {
  // While the controls are up, a tap must not also click whatever menu item
  // happens to sit under the button.
  const int value = enabled ? 1 : 0;
  if (mouse_from_touch_ == value)
    return;
  mouse_from_touch_ = value;
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, enabled ? "1" : "0");
}

void TouchControls::OnDraw(ImGuiIO &io) {
  const bool show = enabled_() && EnsureVirtualPad() && !PhysicalPadConnected();
  if (show != visible_) {
    visible_ = show;
    ReleaseAll();
    SetMouseFromTouch(!show);
  }
  if (!show)
    return;

  const Layout L(io.DisplaySize.x, io.DisplaySize.y);

  // Collect the fingers down right now, keyed so each keeps what it grabbed.
  for (auto &[id, f] : fingers_)
    f.seen = false;
  int device_count = 0;
  SDL_TouchID *devices = SDL_GetTouchDevices(&device_count);
  for (int d = 0; devices && d < device_count; ++d) {
    int finger_count = 0;
    SDL_Finger **list = SDL_GetTouchFingers(devices[d], &finger_count);
    for (int i = 0; list && i < finger_count; ++i) {
      const float px = list[i]->x * L.w, py = list[i]->y * L.h;
      auto [it, added] = fingers_.try_emplace(list[i]->id);
      Finger &f = it->second;
      if (added) {
        f.start_x = px;
        f.start_y = py;
        const bool on_button =
            L.InDpad(px, py) || L.lt.Hit(px, py, 0) || L.lb.Hit(px, py, 0) ||
            L.rt.Hit(px, py, 0) || L.rb.Hit(px, py, 0) ||
            L.back.Hit(px, py, 0) || L.start.Hit(px, py, 0) ||
            std::any_of(std::begin(L.face), std::end(L.face),
                        [&](const Circle &c) { return c.Hit(px, py, 1.3f); });
        f.owner = on_button            ? Owner::kButtons
                  : L.InMoveZone(px, py) ? Owner::kMoveStick
                  : px > L.w * 0.5f      ? Owner::kLookStick
                                         : Owner::kButtons;
      }
      f.x = px;
      f.y = py;
      f.seen = true;
    }
    SDL_free(list);
  }
  SDL_free(devices);
  std::erase_if(fingers_, [](const auto &kv) { return !kv.second.seen; });

  // Resolve every finger into one pad state.
  std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
  std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};
  const Finger *move = nullptr;
  const Finger *look = nullptr;
  const float slack = 0.015f * L.u;
  for (const auto &[id, f] : fingers_) {
    switch (f.owner) {
    case Owner::kMoveStick:
      move = &f;
      break;
    case Owner::kLookStick:
      look = &f;
      break;
    case Owner::kButtons:
      for (int b = 0; b < 4; ++b)
        if (L.face[b].Hit(f.x, f.y, 1.3f))
          buttons[kFace[b].button] = true;
      if (L.lb.Hit(f.x, f.y, slack))
        buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = true;
      if (L.rb.Hit(f.x, f.y, slack))
        buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = true;
      if (L.back.Hit(f.x, f.y, slack))
        buttons[SDL_GAMEPAD_BUTTON_BACK] = true;
      if (L.start.Hit(f.x, f.y, slack))
        buttons[SDL_GAMEPAD_BUTTON_START] = true;
      if (L.lt.Hit(f.x, f.y, slack))
        axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 1.0f;
      if (L.rt.Hit(f.x, f.y, slack))
        axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 1.0f;
      if (L.InDpad(f.x, f.y)) {
        // Eight-way: a thumb between two arms presses both.
        const float dx = f.x - L.dpad_x, dy = f.y - L.dpad_y;
        const float dead = L.dpad_arm * 0.35f;
        if (dy < -dead)
          buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = true;
        if (dy > dead)
          buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = true;
        if (dx < -dead)
          buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = true;
        if (dx > dead)
          buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = true;
      }
      break;
    }
  }

  // Floating sticks: deflection is measured from where the finger landed.
  float mx = 0, my = 0, lx = 0, ly = 0;
  if (move) {
    mx = (move->x - move->start_x) / L.move_radius;
    my = (move->y - move->start_y) / L.move_radius;
    ClampToUnit(mx, my);
  }
  if (look) {
    lx = (look->x - look->start_x) / L.look_radius;
    ly = (look->y - look->start_y) / L.look_radius;
    ClampToUnit(lx, ly);
  }
  axes[SDL_GAMEPAD_AXIS_LEFTX] = mx;
  axes[SDL_GAMEPAD_AXIS_LEFTY] = my; // SDL: positive is down
  axes[SDL_GAMEPAD_AXIS_RIGHTX] = lx;
  axes[SDL_GAMEPAD_AXIS_RIGHTY] = ly;

  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    SDL_SetJoystickVirtualButton(pad_, b, buttons[b]);
  for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
    const bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                         a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    SDL_SetJoystickVirtualAxis(
        pad_, a, trigger ? static_cast<Sint16>(axes[a] * 32767.0f)
                         : ToAxis(axes[a]));
  }

  // Draw.
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const float label = 0.045f * L.u;
  const float small_label = 0.032f * L.u;
  const float stroke = std::max(1.0f, 0.004f * L.u);
  const float round = 0.02f * L.u;

  for (int b = 0; b < 4; ++b) {
    const Circle &c = L.face[b];
    const bool down = buttons[kFace[b].button];
    dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, down ? kFillPressed : kFill);
    dl->AddCircle(ImVec2(c.x, c.y), c.r, kFace[b].color, 0, stroke * 1.5f);
    DrawLabel(dl, label, c.x, c.y, kFace[b].label);
  }

  auto rect_button = [&](const Rect &r, bool down, const char *text) {
    dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1),
                      down ? kFillPressed : kFill, round);
    dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), kEdge, round, 0,
                stroke);
    DrawLabel(dl, small_label, r.Cx(), r.Cy(), text);
  };
  rect_button(L.lt, axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] > 0, "LT");
  rect_button(L.lb, buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER], "LB");
  rect_button(L.rb, buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER], "RB");
  rect_button(L.rt, axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0, "RT");
  rect_button(L.back, buttons[SDL_GAMEPAD_BUTTON_BACK], "BACK");
  rect_button(L.start, buttons[SDL_GAMEPAD_BUTTON_START], "START");

  const float a = L.dpad_arm;
  const struct {
    SDL_GamepadButton button;
    float dx, dy;
  } arms[] = {{SDL_GAMEPAD_BUTTON_DPAD_UP, 0, -1},
              {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0, 1},
              {SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1, 0},
              {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1, 0}};
  for (const auto &arm : arms) {
    const float cx = L.dpad_x + arm.dx * a, cy = L.dpad_y + arm.dy * a;
    const ImVec2 lo(cx - a * 0.5f, cy - a * 0.5f), hi(cx + a * 0.5f, cy + a * 0.5f);
    dl->AddRectFilled(lo, hi, buttons[arm.button] ? kFillPressed : kFill,
                      round * 0.5f);
    dl->AddRect(lo, hi, kEdge, round * 0.5f, 0, stroke);
  }

  // Sticks sit at a resting spot until a finger takes them.
  const float mbx = move ? move->start_x : L.move_rest.x;
  const float mby = move ? move->start_y : L.move_rest.y;
  dl->AddCircleFilled(ImVec2(mbx, mby), L.move_radius, kFill);
  dl->AddCircle(ImVec2(mbx, mby), L.move_radius, kEdge, 0, stroke);
  dl->AddCircleFilled(ImVec2(mbx + mx * L.move_radius, mby + my * L.move_radius),
                      L.move_radius * 0.42f, move ? kFillPressed : kEdge);
  if (look) {
    dl->AddCircle(ImVec2(look->start_x, look->start_y), L.look_radius, kEdge, 0,
                  stroke);
    dl->AddCircleFilled(ImVec2(look->start_x + lx * L.look_radius,
                               look->start_y + ly * L.look_radius),
                        L.look_radius * 0.4f, kFillPressed);
  }
}

} // namespace bd::ui
