/**
 * @file    engine/frame_clock.h
 * @brief   Fixed 30Hz logic tick clock for the fps unlock.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::engine {

void Advance();

// The frame rate the game is paced to: the user's cap, lowered by the
// power-saving mode while it is engaged. 0 = uncapped.
i32 TargetFPS();

bool InterpolationActive();

bool TickDue();

float Alpha();

u64 TickCount();

double TicksPerSecond();

double FrameTime();

double FrameDelta();

void PublishRenderClock();
void BindRenderThread();
bool IsRenderThread();

} // namespace bd::engine
