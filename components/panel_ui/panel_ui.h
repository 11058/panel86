#pragma once

#include "esphome/core/component.h"

#include <cmath>

#include "lvgl.h"
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
  /// Пул картинок под камеры. Передаётся из YAML: свои экземпляры
  /// online_image компонент создать не может, они объявляются заранее.
  /// Запросить свежие кадры со всех показанных камер.
  void refresh_cameras();

  /// Запросить у Home Assistant вычисление всех шаблонов раскладки.
  /// Запрос уходит в отдельную задачу: HTTP блокирует, а главный цикл
  /// рисует. Результат применяется в loop().
  void refresh_templates();

  /// Внутреннее: выполняется в отдельной задаче.
  void fetch_templates_task();

  void add_camera_slot(void *online_image) { this->cam_slots_.push_back(online_image); }

  void set_fonts(const void *title, const void *value, const void *small, const void *icon = nullptr) {
    this->font_title_ = title;
    this->font_value_ = value;
    this->font_small_ = small;
    this->font_icon_ = icon;
    this->font_body_ = value;  // «обычный» и «значение» — один и тот же набор
  }

  // --- Настройки панели -------------------------------------------------
  // Лежат файлом рядом с раскладкой, а НЕ в globals с restore_value.
  // Причина в ADR-0004: ключи хранения ESPHome привязаны к хешу конфига,
  // и обновление прошивки обнуляет сохранённое. Файл переживает всё.

  std::string settings_path() const;
  std::string read_settings();
  bool write_settings(const std::string &data);
  void load_settings();

  // --- Учётные данные Home Assistant --------------------------------------
  // Отдельным файлом, а не в settings.json: настройки выгружают, показывают
  // и держат в git, а токен там делать нечего.
  std::string ha_url();
  std::string ha_token();
  bool save_ha(const std::string &url, const std::string &token);

  /// Растёт при каждом сохранении настроек. По нему прошивка понимает,
  /// что настройки изменились, и применяет их — сами по себе они только
  /// лежат в файле.
  uint32_t settings_revision() const { return this->settings_rev_; }

  int  brightness() const { return this->s_brightness_; }
  int  sleep_brightness() const { return this->s_sleep_brightness_; }
  uint32_t sleep_after_ms() const { return this->s_sleep_after_ms_; }
  bool wake_on_motion() const { return this->s_wake_on_motion_; }
  int  day_start_hour() const { return this->s_day_start_; }
  int  night_start_hour() const { return this->s_night_start_; }
  bool day_always_on() const { return this->s_day_always_on_; }
  bool night_off() const { return this->s_night_off_; }
  const std::string &theme() const { return this->s_theme_; }
  const std::string &font_scale() const { return this->s_font_scale_; }
  const std::string &time_source() const { return this->s_time_source_; }
  const std::string &ntp_server() const { return this->s_ntp_server_; }
  const std::string &timezone() const { return this->s_timezone_; }
  const std::string &sync_every() const { return this->s_sync_every_; }

  // Ethernet
  bool eth_enabled() const { return this->s_eth_enabled_; }
  bool eth_static() const { return this->s_eth_static_; }
  const std::string &eth_ip() const { return this->s_eth_ip_; }
  const std::string &eth_mask() const { return this->s_eth_mask_; }
  const std::string &eth_gw() const { return this->s_eth_gw_; }
  const std::string &eth_dns() const { return this->s_eth_dns_; }

  /// Изменить одну настройку и сразу сохранить. Значение — как в JSON.
  bool set_setting(const std::string &group, const std::string &key, const std::string &value);

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
  /// Одна кнопка в полосе кнопок.
  struct RowButton {
    std::string label;
    std::string entity;
  };

  /// Подкнопка на карточке — маленькая круглая кнопка справа.
  /// В Bubble Card они называются sub-buttons и задаются конфигурацией,
  /// а не зашиты по типу сущности. Здесь так же.
  struct SubButton {
    std::string icon;      // имя MDI
    std::string service;   // что вызвать, например cover.open_cover
    std::string key;       // необязательный параметр
    std::string value;
    std::string label;     // подпись — нужна кнопкам во всплывашке
  };

  /// Одна строка во всплывающих подробностях. Позволяет собрать окно
  /// из чего угодно, а не только из набора, заданного типом карточки.
  struct PopupRow {
    std::string kind;      // slider | pill | buttons | text
    std::string label;
    std::string service;
    std::string key;
    int lo{0};
    int hi{100};
    int step{1};
    std::vector<SubButton> buttons;
  };

  struct Card {
    std::string type;
    std::string entity;
    std::string label;
    std::string unit;
    std::string icon_name;
    int decimals{1};
    int span_w{1};
    int span_h{1};
    float deadband{0.0f};       // не перерисовывать при меньшем изменении
    uint32_t throttle_ms{1000}; // и не чаще, чем раз в столько
    float warn_above{NAN};
    float warn_below{NAN};

    void *box{nullptr};        // lv_obj_t *
    void *lbl_name{nullptr};   // lv_obj_t *
    void *lbl_value{nullptr};  // lv_obj_t *
    void *lbl_sub{nullptr};    // lv_obj_t * — вторая строка: уставка, режим
    void *icon{nullptr};       // lv_obj_t * — символ в круге
    void *icon_box{nullptr};   // lv_obj_t * — сам круг
    void *fill{nullptr};       // lv_obj_t * — заливка по яркости
    void *ctl_value{nullptr};  // lv_obj_t * — значение в органах управления на карточке
    void *spark{nullptr};      // lv_obj_t * — спарклайн у датчика
    bool  want_graph{false};
    std::vector<float> history;   // накопленные значения для спарклайна
    std::vector<lv_point_precise_t> spark_pts;  // точки линии, живут вместе с карточкой
    int   level{-1};           // яркость 0..100, -1 если неизвестно
    bool  active{false};
    std::string state;

    // Живые значения атрибутов. Нужны, чтобы подробности открывались
    // на текущих значениях, а не на выдуманных: иначе ползунок уставки
    // показывает 22, человек его трогает — и климат уезжает с 18 на 22.
    float target_temp{NAN};
    float current_temp{NAN};
    int   position{-1};        // штора, 0..100
    int   volume{-1};          // медиа, 0..100
    int   fan_pct{-1};
    int   color_temp{-1};      // кельвины
    int32_t rgb{-1};           // цвет лампы, 0xRRGGBB
    std::vector<RowButton> buttons;
    std::vector<SubButton> sub_buttons;

    // Шаблоны Home Assistant. Вычисляются на стороне HA — панели такое
    // не под силу, да и незачем: у HA уже есть все состояния и Jinja.
    std::string tpl_label;
    std::string tpl_state;
    std::string tpl_icon;
    std::vector<PopupRow> popup;
    void *cam_slot{nullptr};   // online_image::OnlineImage *
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

  /// Нажатие на иконку: открыть подробности. Как в Bubble Card — само
  /// нажатие переключает, а подробности прячутся за иконкой, чтобы
  /// не мешать основному действию.
  void open_details(Card *card);
  void close_details();

  /// Вызвать действие Home Assistant с одним параметром сверх entity_id.
  void call_service(const std::string &service, const char *key = nullptr,
                    const std::string &value = "");

  /// Вызвать действие для конкретной сущности — для полосы кнопок,
  /// где у каждой кнопки своя.
  void call_service_for(const std::string &entity, const std::string &service);
  void call_service_for_with(const std::string &entity, const std::string &service,
                             const std::string &key, const std::string &value);

  size_t card_count() const { return this->cards_.size(); }

  /// Перестроить интерфейс из того, что сейчас лежит в файле.
  /// Используется после заливки новой раскладки по HTTP.
  bool rebuild();

  /// Назначить перестроение. Безопасно вызывать из любой задачи: сама
  /// работа произойдёт в главном цикле.
  void request_rebuild() { this->rebuild_pending_ = true; }

 protected:
  void render_page_(void *tile, const void *page_json, int w, int h);
  void render_card_(void *parent, Card *card, int x, int y, int w, int h);
  void build_inline_controls(void *box, Card *card, int w, int h, int ctl_w);
  void build_step_pill(void *parent, Card *card, int x, int y, int w, int h,
                       const std::string &service, const std::string &key, int lo, int hi,
                       int step_v);
  void update_card_value_(Card *card, const std::string &state);
  void update_card_level_(Card *card, int level);
  void draw_spark_(Card *card);

 public:
  /// Перекрасить карточку под текущий цвет лампы. Публично, потому что
  /// вызывается из обработчика подписки.
  void refresh_card_colors(Card *card);

 protected:
  void apply_state_(Card *card, const std::string &state);
  void start_http_();
  void build_details_controls(void *sheet, Card *card, int width);

  std::vector<Card *> cards_;
  std::vector<void *> cam_slots_;   // esphome::online_image::OnlineImage *
  size_t cam_used_{0};
  std::vector<void *> dots_;   // lv_obj_t * — индикатор страниц
  void *scroller_{nullptr};    // lv_obj_t *
  void *sheet_{nullptr};       // lv_obj_t * — панель подробностей
  Card *sheet_card_{nullptr};
  const char *partition_{"storage"};
  const char *base_path_{"/fs"};
  const char *layout_file_{"layout.json"};
  const char *settings_file_{"settings.json"};
  const char *ha_file_{"ha.json"};
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
  const void *font_icon_{nullptr};
  const void *font_body_{nullptr};
  void *root_{nullptr};   // lv_obj_t *
  void *httpd_{nullptr};  // httpd_handle_t
  bool mounted_{false};
  bool http_started_{false};
  bool animate_build_{true};
  // Перестроение назначается из задачи веб-сервера, а выполняется в главном
  // цикле: LVGL не потокобезопасен, и трогать его виджеты из другой задачи
  // одновременно с отрисовкой — гарантированная гонка.
  volatile bool rebuild_pending_{false};
  volatile bool tpl_ready_{false};
  volatile bool tpl_busy_{false};
  std::string tpl_request_;
  std::string tpl_result_;
  std::vector<std::pair<Card *, int>> tpl_targets_;  // 0 подпись, 1 состояние, 2 иконка
  uint32_t settings_rev_{0};

  // Разобранные настройки
  int s_brightness_{80};
  int s_sleep_brightness_{10};
  uint32_t s_sleep_after_ms_{60000};
  bool s_wake_on_motion_{true};
  int  s_day_start_{7};        // час начала «дня»
  int  s_night_start_{23};     // час начала «ночи»
  bool s_day_always_on_{false};
  bool s_night_off_{true};
  std::string s_theme_{"dark"};
  std::string s_font_scale_{"normal"};
  std::string s_time_source_{"sntp"};
  std::string s_ntp_server_{"pool.ntp.org"};
  std::string s_timezone_{"UTC-5"};
  std::string s_sync_every_{"6h"};
  bool s_eth_enabled_{false};
  bool s_eth_static_{false};
  std::string s_eth_ip_;
  std::string s_eth_mask_{"255.255.255.0"};
  std::string s_eth_gw_;
  std::string s_eth_dns_;
};

}  // namespace panel_ui
}  // namespace esphome
