// Сгенерировано tools/gen_icons.py. Не править вручную.
//
// Имена соответствуют Material Design Icons: те же, что в Home Assistant
// после "mdi:". Это важно — человек, выбирающий иконку, уже знает их
// по интерфейсу HA и не должен учить второй набор названий.
#pragma once

#include <cstring>

namespace esphome {
namespace panel_ui {

struct IconEntry {
  const char *name;
  const char *utf8;
};

static const IconEntry ICON_TABLE[] = {
    {"air-conditioner", "\xF3\xB0\x80\x9B"},
    {"alert", "\xF3\xB0\x80\xA6"},
    {"battery", "\xF3\xB0\x81\xB9"},
    {"blinds", "\xF3\xB0\x82\xAC"},
    {"camera", "\xF3\xB0\x84\x80"},
    {"cctv", "\xF3\xB0\x9E\xAE"},
    {"ceiling-light", "\xF3\xB0\x9D\xA9"},
    {"chart-line", "\xF3\xB0\x84\xAA"},
    {"cog", "\xF3\xB0\x92\x93"},
    {"counter", "\xF3\xB0\x86\x99"},
    {"curtains", "\xF3\xB1\xA1\x86"},
    {"door", "\xF3\xB0\xA0\x9A"},
    {"dots-horizontal", "\xF3\xB0\x87\x99"},
    {"eye", "\xF3\xB0\x88\x88"},
    {"fan", "\xF3\xB0\x88\x90"},
    {"fire", "\xF3\xB0\x88\xB8"},
    {"flash", "\xF3\xB0\x89\x81"},
    {"fridge", "\xF3\xB0\x8A\x90"},
    {"garage", "\xF3\xB0\x9B\x99"},
    {"gate", "\xF3\xB0\x8A\x99"},
    {"gauge", "\xF3\xB0\x8A\x9A"},
    {"home", "\xF3\xB0\x8B\x9C"},
    {"lamp", "\xF3\xB0\x9A\xB5"},
    {"leaf", "\xF3\xB0\x8C\xAA"},
    {"led-strip", "\xF3\xB0\x9F\x96"},
    {"lightbulb", "\xF3\xB0\x8C\xB5"},
    {"lock", "\xF3\xB0\x8C\xBE"},
    {"lock-open-variant", "\xF3\xB0\xBF\x86"},
    {"motion-sensor", "\xF3\xB0\xB6\x91"},
    {"movie-open", "\xF3\xB0\xBF\x8E"},
    {"music", "\xF3\xB0\x9D\x9A"},
    {"palette", "\xF3\xB0\x8F\x98"},
    {"pause", "\xF3\xB0\x8F\xA4"},
    {"pipe", "\xF3\xB0\x9F\xA5"},
    {"play", "\xF3\xB0\x90\x8A"},
    {"power", "\xF3\xB0\x90\xA5"},
    {"power-plug", "\xF3\xB0\x9A\xA5"},
    {"radiator", "\xF3\xB0\xB6\x8B"},
    {"robot-vacuum", "\xF3\xB0\x9C\x8D"},
    {"script-text", "\xF3\xB0\xAF\x81"},
    {"smoke-detector", "\xF3\xB0\x8E\x92"},
    {"snowflake", "\xF3\xB0\x9C\x97"},
    {"speaker", "\xF3\xB0\x93\x83"},
    {"sprinkler-variant", "\xF3\xB1\x88\x92"},
    {"television", "\xF3\xB0\x94\x82"},
    {"thermometer", "\xF3\xB0\x94\x8F"},
    {"thermostat", "\xF3\xB0\x8E\x93"},
    {"toggle-switch", "\xF3\xB0\x94\xA1"},
    {"valve", "\xF3\xB1\x81\xA6"},
    {"washing-machine", "\xF3\xB0\x9C\xAA"},
    {"water", "\xF3\xB0\x96\x8C"},
    {"water-percent", "\xF3\xB0\x96\x8E"},
    {"water-pump", "\xF3\xB1\x89\x8F"},
    {"weather-night", "\xF3\xB0\x96\x94"},
    {"weather-sunny", "\xF3\xB0\x96\x99"},
    {"wifi", "\xF3\xB0\x96\xA9"},
    {"window-shutter", "\xF3\xB0\xBF\x9D"},
};
static const size_t ICON_COUNT = sizeof(ICON_TABLE) / sizeof(ICON_TABLE[0]);

inline const char *icon_by_name(const char *name) {
  if (name == nullptr || *name == 0)
    return nullptr;
  for (size_t i = 0; i < ICON_COUNT; i++)
    if (strcmp(ICON_TABLE[i].name, name) == 0)
      return ICON_TABLE[i].utf8;
  return nullptr;
}

}  // namespace panel_ui
}  // namespace esphome
