#pragma once

#include "esphome/core/component.h"

#include <cmath>
#include <string>
#include <vector>

namespace esphome {
namespace panel_ui {

/// Хранилище раскладки интерфейса на LittleFS.
///
/// Пока это только слой хранения: смонтировать раздел, прочитать и записать
/// layout.json. Интерпретатор JSON в дерево LVGL — следующий этап.
class PanelUI : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_partition(const char *label) { this->partition_ = label; }
  void set_base_path(const char *path) { this->base_path_ = path; }
  void set_layout_file(const char *name) { this->layout_file_ = name; }
  void set_http_port(uint16_t port) { this->http_port_ = port; }

  /// Контейнер, в который строится интерфейс. Запоминается, чтобы
  /// перестроение по HTTP не требовало передавать его заново.
  void set_root(void *root) { this->root_ = root; }

  /// Шрифты для карточек. Принимаем lv_font_t* как void*, чтобы заголовок
  /// не тянул lvgl.h. Из YAML: id(f_title)->get_lv_font() и т.д.
  void set_fonts(const void *title, const void *value, const void *small) {
    this->font_title_ = title;
    this->font_value_ = value;
    this->font_small_ = small;
  }

  /// Заполнить карточки правдоподобными значениями без Home Assistant.
  /// Нужно, чтобы проверять вёрстку и типы карточек, когда HA недоступен.
  void demo_fill();

  /// Подсветить точку активной страницы. Вызывается из обработчика прокрутки.
  void sync_dots();

  bool is_mounted() const { return this->mounted_; }

  /// Полный путь к файлу раскладки.
  std::string layout_path() const;

  /// Прочитать раскладку. Пустая строка — файла нет или не читается.
  std::string read_layout();

  /// Записать раскладку целиком. Возвращает успех.
  bool write_layout(const std::string &data);

  /// Занято и всего байт в файловой системе.
  bool fs_usage(size_t *used, size_t *total);

  /// Одна карточка на экране: что показывает и чем нарисована.
  struct Card {
    std::string type;
    std::string entity;
    std::string label;
    std::string unit;
    int decimals{1};
    float deadband{0.0f};       // не перерисовывать при меньшем изменении
    uint32_t throttle_ms{1000}; // и не чаще, чем раз в столько
    float warn_above{NAN};
    float warn_below{NAN};

    void *box{nullptr};        // lv_obj_t *
    void *lbl_name{nullptr};   // lv_obj_t *
    void *lbl_value{nullptr};  // lv_obj_t *
    void *lbl_sub{nullptr};    // lv_obj_t * — вторая строка: уставка, режим
    PanelUI *owner{nullptr};

    // Состояние подавления дребезга
    float last_num{NAN};
    uint32_t last_draw{0};
  };

  /// Разобрать раскладку и построить интерфейс внутри контейнера.
  /// root — это lv_obj_t*, тип размыт намеренно, чтобы заголовок
  /// не тянул за собой lvgl.h.
  bool build_ui(void *root);

  /// Подписаться на сущности всех построенных карточек.
  /// Работает только после переподключения к API — отписки в протоколе
  /// нет, см. docs/adr/0002-privyazka-v-runtime.md.
  void bind_entities();

  /// Нажатие на карточку: переключить сущность в Home Assistant.
  void on_card_tapped(Card *card);

  size_t card_count() const { return this->cards_.size(); }

  /// Перестроить интерфейс из того, что сейчас лежит в файле.
  /// Используется после заливки новой раскладки по HTTP.
  bool rebuild();

 protected:
  void render_page_(void *tile, const void *page_json, int w, int h);
  void render_card_(void *parent, Card *card, int x, int y, int w, int h);
  void update_card_value_(Card *card, const std::string &state);
  void apply_state_(Card *card, const std::string &state);
  void start_http_();

  std::vector<Card *> cards_;
  std::vector<void *> dots_;   // lv_obj_t * — индикатор страниц
  void *scroller_{nullptr};    // lv_obj_t *
  const char *partition_{"storage"};
  const char *base_path_{"/fs"};
  const char *layout_file_{"layout.json"};
  uint16_t http_port_{8080};
  // Тема из раскладки
  uint32_t theme_accent_{0xC2610C};
  int theme_radius_{16};
  bool theme_accent_set_{false};
  float def_deadband_{0.0f};
  std::string def_throttle_{"1s"};

  const void *font_title_{nullptr};
  const void *font_value_{nullptr};
  const void *font_small_{nullptr};
  void *root_{nullptr};   // lv_obj_t *
  void *httpd_{nullptr};  // httpd_handle_t
  bool mounted_{false};
  bool http_started_{false};
};

}  // namespace panel_ui
}  // namespace esphome
