#pragma once

#include <Arduino.h>
#include <helpers/ui/DisplayDriver.h>

namespace NeonPocketMono {

static constexpr unsigned long DURATION_MILLIS = 3200;
static constexpr unsigned long FRAME_MILLIS = 80;

inline void drawFrame(DisplayDriver& display, unsigned long elapsed,
                      const char* device, const char* version) {
  if (elapsed > DURATION_MILLIS) elapsed = DURATION_MILLIS;
  const uint8_t frame = elapsed / FRAME_MILLIS;
  const int progress = (int)((elapsed * 116UL) / DURATION_MILLIS);

  display.setColor(UIColor::secondary_txt);
  for (uint8_t i = 0; i < 14; ++i) {
    const int x = (i * 29 + frame * (1 + (i % 3))) % 128;
    const int y = 3 + ((i * 17 + frame / 2) % 51);
    display.fillRect(x, y, (i % 5) == 0 ? 2 : 1, 1);
  }

  display.setColor(UIColor::corp_blue);
  display.fillRect(4, 14, 2, 27);
  display.fillRect(16, 14, 2, 27);
  display.fillRect(6, 16, 2, 4);
  display.fillRect(8, 20, 2, 5);
  display.fillRect(10, 25, 2, 5);
  display.fillRect(12, 30, 2, 5);
  display.fillRect(14, 35, 2, 4);
  display.fillRect(22, 14, 2, 27);
  display.fillRect(24, 14, 9, 2);
  display.fillRect(31, 16, 2, 10);
  display.fillRect(24, 25, 9, 2);

  const int scan_y = 13 + ((frame * 3) % 30);
  display.fillRect(2, scan_y, 34, 1);

  display.setTextSize(1);
  display.setColor(UIColor::primary_txt);
  display.setCursor(40, 13);
  display.print("NEONPOCKET");
  display.setColor(UIColor::secondary_txt);
  display.setCursor(40, 27);
  display.print(device);
  display.setCursor(40, 39);
  display.print(version);

  display.setColor(UIColor::corp_blue);
  display.drawRect(4, 56, 120, 6);
  if (progress > 0) display.fillRect(6, 58, progress, 2);
  const int beam_x = 6 + progress;
  if (beam_x < 123) display.fillRect(beam_x, 55, 2, 8);
}

} // namespace NeonPocketMono
