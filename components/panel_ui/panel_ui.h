#pragma once

#include "esphome/core/component.h"
#include <string>

namespace esphome {
namespace panel_ui {

/// Хранилище раскладки интерфейса на LittleFS.
///
/// Пока это только слой хранения: смонтировать раздел, прочитать и записать
/// layout.json. Интерпретатор JSON в дерево LVGL — следующий этап.
class PanelUI : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_partition(const char *label) { this->partition_ = label; }
  void set_base_path(const char *path) { this->base_path_ = path; }
  void set_layout_file(const char *name) { this->layout_file_ = name; }

  bool is_mounted() const { return this->mounted_; }

  /// Полный путь к файлу раскладки.
  std::string layout_path() const;

  /// Прочитать раскладку. Пустая строка — файла нет или не читается.
  std::string read_layout();

  /// Записать раскладку целиком. Возвращает успех.
  bool write_layout(const std::string &data);

  /// Занято и всего байт в файловой системе.
  bool fs_usage(size_t *used, size_t *total);

 protected:
  const char *partition_{"storage"};
  const char *base_path_{"/fs"};
  const char *layout_file_{"layout.json"};
  bool mounted_{false};
};

}  // namespace panel_ui
}  // namespace esphome
