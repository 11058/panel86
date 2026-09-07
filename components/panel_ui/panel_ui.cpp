#include "panel_ui.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>

#include "esp_littlefs.h"
#include "esp_http_server.h"

#include "editor_html.h"
#include "icons_mdi.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include <array>
#include <initializer_list>
#include <utility>
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "esphome/components/network/util.h"

#include "esphome/components/json/json_util.h"
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/api_pb2.h"
#endif
#include "lvgl.h"

namespace esphome {
namespace panel_ui {

static const char *const TAG = "panel_ui";

/// "250ms", "2s" -> миллисекунды. Пустое или мусор -> 1000.
static uint32_t parse_ms(const char *v) {
  if (v == nullptr || *v == 0)
    return 1000;
  char *end = nullptr;
  const long n = strtol(v, &end, 10);
  if (end == v || n < 0)
    return 1000;
  if (end != nullptr && *end == 's')
    return static_cast<uint32_t>(n) * 1000;
  return static_cast<uint32_t>(n);
}

void PanelUI::setup() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = this->base_path_;
  conf.partition_label = this->partition_;
  conf.format_if_mount_failed = true;  // первое включение: раздел пустой
  conf.dont_mount = false;

  // Проверяем ДО монтирования, есть ли уже размеченная файловая система:
  // format_if_mount_failed молча стирает раздел при сбое монтирования,
  // и это выглядит как «настройки не сохраняются».
  conf.dont_mount = true;
  const bool premount_ok = esp_vfs_littlefs_register(&conf) == ESP_OK;
  if (premount_ok)
    esp_vfs_littlefs_unregister(this->partition_);
  conf.dont_mount = false;

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LittleFS не смонтирован на разделе '%s': %s", this->partition_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->mounted_ = true;
  ESP_LOGI(TAG, "LittleFS: раздел %s размечен ранее", premount_ok ? "был" : "НЕ был (значит его отформатировали сейчас)");

  size_t used = 0, total = 0;
  if (this->fs_usage(&used, &total)) {
    ESP_LOGI(TAG, "LittleFS смонтирован: %u из %u КиБ занято", (unsigned) (used / 1024), (unsigned) (total / 1024));
  }

  this->load_settings();

  std::string layout = this->read_layout();
  if (layout.empty()) {
    ESP_LOGW(TAG, "Раскладки нет — панель не настроена");
  } else {
    ESP_LOGI(TAG, "Раскладка найдена: %u байт", (unsigned) layout.size());
  }
}

void PanelUI::loop() {
  // Перестроение, назначенное из веб-сервера. Здесь мы в главном цикле,
  // и трогать LVGL безопасно.
  if (this->rebuild_pending_) {
    this->rebuild_pending_ = false;
    ESP_LOGI(TAG, "перестраиваю интерфейс по новой раскладке");
    this->rebuild();
  }

  // HTTP-сервер нельзя поднимать в setup(): там ещё не инициализирован
  // сетевой стек, и httpd падает с assert failed: xQueueSemaphoreTake.
  // Поэтому лениво, при первом появлении сети.
  if (!this->http_started_ && this->mounted_ && network::is_connected()) {
    this->start_http_();
    this->http_started_ = true;
  }
}

void PanelUI::dump_config() {
  ESP_LOGCONFIG(TAG, "Panel UI:");
  ESP_LOGCONFIG(TAG, "  Раздел: %s", this->partition_);
  ESP_LOGCONFIG(TAG, "  Путь: %s", this->layout_path().c_str());
  ESP_LOGCONFIG(TAG, "  Смонтировано: %s", YESNO(this->mounted_));
  size_t used = 0, total = 0;
  if (this->mounted_ && this->fs_usage(&used, &total)) {
    ESP_LOGCONFIG(TAG, "  Занято: %u из %u КиБ", (unsigned) (used / 1024), (unsigned) (total / 1024));
  }
}

std::string PanelUI::layout_path() const {
  return std::string(this->base_path_) + "/" + this->layout_file_;
}

bool PanelUI::fs_usage(size_t *used, size_t *total) {
  if (!this->mounted_)
    return false;
  return esp_littlefs_info(this->partition_, total, used) == ESP_OK;
}

std::string PanelUI::read_layout() {
  if (!this->mounted_)
    return {};
  const std::string path = this->layout_path();
  FILE *f = fopen(path.c_str(), "rb");
  if (f == nullptr)
    return {};

  std::string out;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  fclose(f);
  return out;
}

bool PanelUI::write_layout(const std::string &data) {
  if (!this->mounted_) {
    ESP_LOGE(TAG, "Запись невозможна: файловая система не смонтирована");
    return false;
  }
  // Пишем во временный файл и переименовываем: обрыв питания посреди
  // записи не должен оставить панель с половиной раскладки.
  const std::string path = this->layout_path();
  const std::string tmp = path + ".tmp";

  FILE *f = fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Не открывается на запись: %s", tmp.c_str());
    return false;
  }
  const size_t written = fwrite(data.data(), 1, data.size(), f);
  fclose(f);

  if (written != data.size()) {
    ESP_LOGE(TAG, "Записано %u из %u байт", (unsigned) written, (unsigned) data.size());
    remove(tmp.c_str());
    return false;
  }
  errno = 0;
  int rm = remove(path.c_str());
  int rm_errno = errno;
  errno = 0;
  int rn = rename(tmp.c_str(), path.c_str());
  if (rn != 0) {
    ESP_LOGE(TAG, "rename('%s' -> '%s') = %d, errno=%d (%s); предыдущий remove=%d errno=%d", tmp.c_str(),
             path.c_str(), rn, errno, strerror(errno), rm, rm_errno);
    return false;
  }
  ESP_LOGI(TAG, "Раскладка сохранена: %u байт", (unsigned) data.size());
  return true;
}

// ---------------------------------------------------------------------------
// Настройки панели: файл рядом с раскладкой
// ---------------------------------------------------------------------------

std::string PanelUI::settings_path() const {
  return std::string(this->base_path_) + "/" + this->settings_file_;
}

std::string PanelUI::read_settings() {
  if (!this->mounted_)
    return {};
  FILE *f = fopen(this->settings_path().c_str(), "rb");
  if (f == nullptr)
    return {};
  std::string out;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  fclose(f);
  return out;
}

bool PanelUI::write_settings(const std::string &data) {
  if (!this->mounted_)
    return false;
  const std::string path = this->settings_path();
  const std::string tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (f == nullptr)
    return false;
  const size_t written = fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  if (written != data.size()) {
    remove(tmp.c_str());
    return false;
  }
  remove(path.c_str());
  if (rename(tmp.c_str(), path.c_str()) != 0)
    return false;
  ESP_LOGI(TAG, "Настройки сохранены: %u байт", (unsigned) data.size());
  this->settings_rev_++;
  this->load_settings();
  return true;
}

// Чтение и запись учётных данных HA. Токен хранится на панели по прямой
// просьбе владельца: иначе он теряется при каждой смене браузера
// или очистке хранилища, и список сущностей приходится подключать заново.
// В ответах наружу токен НЕ отдаётся — только признак, что он задан.
static std::string read_file_(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (f == nullptr)
    return {};
  std::string out;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  fclose(f);
  return out;
}

std::string PanelUI::ha_url() {
  std::string url;
  json::parse_json(read_file_(std::string(this->base_path_) + "/" + this->ha_file_),
                   [&](JsonObject o) -> bool { url = std::string(o["url"] | ""); return true; });
  return url;
}

std::string PanelUI::ha_token() {
  std::string tok;
  json::parse_json(read_file_(std::string(this->base_path_) + "/" + this->ha_file_),
                   [&](JsonObject o) -> bool { tok = std::string(o["token"] | ""); return true; });
  return tok;
}

bool PanelUI::save_ha(const std::string &url, const std::string &token) {
  if (!this->mounted_)
    return false;
  const std::string path = std::string(this->base_path_) + "/" + this->ha_file_;
  const std::string tmp = path + ".tmp";
  std::string body = "{\"url\":\"" + url + "\",\"token\":\"" + token + "\"}";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (f == nullptr)
    return false;
  const size_t written = fwrite(body.data(), 1, body.size(), f);
  fclose(f);
  if (written != body.size()) {
    remove(tmp.c_str());
    return false;
  }
  remove(path.c_str());
  if (rename(tmp.c_str(), path.c_str()) != 0)
    return false;
  ESP_LOGI(TAG, "учётные данные Home Assistant сохранены (%s)", url.c_str());
  return true;
}

void PanelUI::load_settings() {
  const std::string data = this->read_settings();
  if (data.empty()) {
    ESP_LOGI(TAG, "Настроек нет, беру умолчания");
    return;
  }
  json::parse_json(data, [this](JsonObject doc) -> bool {
    JsonObject d = doc["display"].as<JsonObject>();
    if (!d.isNull()) {
      this->s_brightness_ = d["brightness"] | 80;
      this->s_sleep_brightness_ = d["sleep_brightness"] | 10;
      this->s_sleep_after_ms_ = parse_ms(d["sleep_after"] | "60s");
      this->s_wake_on_motion_ = d["wake_on_motion"] | true;
      this->s_theme_ = std::string(d["theme"] | "dark");
      this->s_font_scale_ = std::string(d["font_scale"] | "normal");
      this->s_day_start_ = d["day_start"] | 7;
      this->s_night_start_ = d["night_start"] | 23;
      this->s_day_always_on_ = d["day_always_on"] | false;
      this->s_night_off_ = d["night_off"] | true;
    }
    JsonObject t = doc["time"].as<JsonObject>();
    if (!t.isNull()) {
      this->s_time_source_ = std::string(t["source"] | "sntp");
      this->s_ntp_server_ = std::string(t["server"] | "pool.ntp.org");
      this->s_timezone_ = std::string(t["timezone"] | "UTC-5");
      this->s_sync_every_ = std::string(t["sync_every"] | "6h");
    }
    return true;
  });
  ESP_LOGI(TAG, "Настройки: яркость %d%%, засыпание %u мс, тема %s, время из %s",
           this->s_brightness_, (unsigned) this->s_sleep_after_ms_, this->s_theme_.c_str(),
           this->s_time_source_.c_str());
}

bool PanelUI::set_setting(const std::string &group, const std::string &key, const std::string &value) {
  std::string data = this->read_settings();
  if (data.empty())
    data = "{}";

  // Читаем, правим одно поле, пишем обратно. Файл маленький, так что
  // перезапись целиком проще и надёжнее точечного редактирования.
  JsonDocument doc;
  if (deserializeJson(doc, data) != DeserializationError::Ok)
    doc.clear();

  JsonObject grp = doc[group].isNull() ? doc[group].to<JsonObject>() : doc[group].as<JsonObject>();

  // Число или строка — решаем по содержимому, чтобы в файле не заводились
  // числа в кавычках.
  char *end = nullptr;
  const long num = strtol(value.c_str(), &end, 10);
  if (end != value.c_str() && end != nullptr && *end == 0) {
    grp[key] = num;
  } else if (value == "true" || value == "false") {
    grp[key] = (value == "true");
  } else {
    grp[key] = value;
  }

  std::string out;
  serializeJson(doc, out);
  return this->write_settings(out);
}

// ---------------------------------------------------------------------------
// Интерпретатор: JSON -> дерево LVGL
// ---------------------------------------------------------------------------

// Цвет акцента по типу карточки. Пока грубо: смысл в том, чтобы типы
// различались на экране, а не в красоте — тема появится позже.
// Оттенок по типу. Акцент темы задаёт управляемые элементы, измеряемые
// остаются нейтральными — так на экране видно, что можно трогать.
static uint32_t g_accent = 0xC2610C;

// Цвета карточек зависят от выбранной схемы. Зашивать их нельзя:
// на светлой схеме тёмная карточка со светлым текстом нечитаема.
static bool g_dark = true;
static uint32_t card_bg()      { return g_dark ? 0x161C22 : 0xFFFFFF; }
static uint32_t card_bg_page() { return g_dark ? 0x0B0F13 : 0xF2F4F6; }
static uint32_t card_ink()     { return g_dark ? 0xE3E9EE : 0x141C24; }
static uint32_t card_ink2()    { return g_dark ? 0x9AA8B4 : 0x4E5D6B; }
static uint32_t card_ink3()    { return g_dark ? 0x6B7B88 : 0x7C8B99; }

static lv_color_t accent_for(const std::string &type) {
  if (type == "light" || type == "switch" || type == "scene" || type == "script")
    return lv_color_hex(g_accent);
  if (type == "climate") return lv_color_hex(0xA83232);
  if (type == "valve" || type == "cover") return lv_color_hex(0x2E6DA4);
  return lv_color_hex(0x5A6875);
}

// Активная страница определяется по положению прокрутки.
static void scroll_event_cb(lv_event_t *e) {
  auto *self = static_cast<PanelUI *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->sync_dots();
}

static void icon_event_cb(lv_event_t *e) {
  auto *card = static_cast<PanelUI::Card *>(lv_event_get_user_data(e));
  if (card != nullptr && card->owner != nullptr)
    card->owner->open_details(card);
}

static void card_event_cb(lv_event_t *e) {
  auto *card = static_cast<PanelUI::Card *>(lv_event_get_user_data(e));
  if (card != nullptr && card->owner != nullptr)
    card->owner->on_card_tapped(card);
}

// Символ для карточки. Берём встроенные символы LVGL, чтобы не тащить
// в прошивку отдельный шрифт иконок: своих глифов у кириллических шрифтов
// нет, а montserrat символы содержит.
// Кнопка в подробностях: подпись плюс действие. Действие хранится в самой
// кнопке, чтобы не заводить отдельную структуру на каждую.
struct SheetAction {
  PanelUI *self;
  std::string service;
  std::string key;
  std::string value;
};

// Кнопка в полосе: сущность лежит в value, потому что у каждой кнопки
// она своя, в отличие от подробностей, где сущность общая для панели.
static void buttons_row_cb(lv_event_t *e) {
  auto *a = static_cast<SheetAction *>(lv_event_get_user_data(e));
  if (a == nullptr || a->value.empty())
    return;
  a->self->call_service_for(a->value, a->service);
}

static void sheet_action_cb(lv_event_t *e) {
  auto *a = static_cast<SheetAction *>(lv_event_get_user_data(e));
  if (a == nullptr)
    return;
  a->self->call_service(a->service, a->key.empty() ? nullptr : a->key.c_str(), a->value);
}

static void sheet_action_free_cb(lv_event_t *e) {
  delete static_cast<SheetAction *>(lv_event_get_user_data(e));
}

// Иконка карточки. Своя из раскладки имеет приоритет, иначе разумная
// по типу — чтобы карточка выглядела осмысленно сразу после добавления.
static const char *icon_for(const std::string &type, const std::string &custom) {
  if (const char *own = icon_by_name(custom.c_str()))
    return own;
  const char *name = "dots-horizontal";
  if (type == "light")   name = "lightbulb";
  else if (type == "switch")  name = "toggle-switch";
  else if (type == "climate") name = "radiator";
  else if (type == "valve")   name = "valve";
  else if (type == "cover")   name = "window-shutter";
  else if (type == "media")   name = "play";
  else if (type == "camera")  name = "cctv";
  else if (type == "lock")    name = "lock";
  else if (type == "scene")   name = "movie-open";
  else if (type == "script")  name = "script-text";
  else if (type == "sensor")  name = "gauge";
  const char *r = icon_by_name(name);
  return r != nullptr ? r : "";
}

void PanelUI::render_card_(void *parent, Card *card, int x, int y, int w, int h) {
  auto *par = static_cast<lv_obj_t *>(parent);
  const lv_color_t accent = accent_for(card->type);

  // Разделитель: подпись и линия, без рамки и фона. Нужен, чтобы делить
  // страницу на смысловые части — «свет», «климат», — а не сваливать
  // всё в один ряд одинаковых пузырей.
  if (card->type == "separator") {
    lv_obj_t *wrap = lv_obj_create(par);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_pos(wrap, x, y);
    lv_obj_set_size(wrap, w, h);

    lv_obj_t *lbl = lv_label_create(wrap);
    lv_label_set_text(lbl, card->label.c_str());
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(card_ink2()), LV_PART_MAIN);
    if (this->font_small_ != nullptr)
      lv_obj_set_style_text_font(lbl, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);

    lv_obj_t *line = lv_obj_create(wrap);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, w - 12, 2);
    lv_obj_align(line, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_obj_set_style_bg_color(line, lv_color_hex(card_ink3()), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, LV_PART_MAIN);

    card->box = wrap;
    card->owner = this;
    return;
  }

  // Полоса кнопок: несколько сцен или сценариев в один ряд. По отдельной
  // карточке на каждую сцену уходит вся страница, а нажимают их редко.
  if (card->type == "buttons") {
    lv_obj_t *wrap = lv_obj_create(par);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_pos(wrap, x, y);
    lv_obj_set_size(wrap, w, h);

    const int n = static_cast<int>(card->buttons.size());
    if (n > 0) {
      const int gap = 10;
      const int bw = (w - gap * (n - 1)) / n;
      for (int i = 0; i < n; i++) {
        auto &b = card->buttons[i];
        lv_obj_t *btn = lv_button_create(wrap);
        lv_obj_set_size(btn, bw, h);
        lv_obj_set_pos(btn, i * (bw + gap), 0);
        lv_obj_set_style_radius(btn, h / 2 > 30 ? 30 : h / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, accent, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);

        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, b.label.c_str());
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, bw - 16);
        lv_obj_center(l);
        lv_obj_set_style_text_color(l, lv_color_hex(card_ink()), LV_PART_MAIN);
        if (this->font_small_ != nullptr)
          lv_obj_set_style_text_font(l, static_cast<const lv_font_t *>(this->font_small_),
                                     LV_PART_MAIN);

        const std::string domain = b.entity.substr(0, b.entity.find('.'));
        auto *act = new SheetAction{this, domain + ".turn_on", "", b.entity};  // NOLINT
        lv_obj_add_event_cb(btn, buttons_row_cb, LV_EVENT_CLICKED, act);
        lv_obj_add_event_cb(btn, sheet_action_free_cb, LV_EVENT_DELETE, act);
      }
    }
    card->box = wrap;
    card->owner = this;
    return;
  }

  // «Пузырь»: сильное скругление, без рамки, мягкий фон. Форма и есть
  // основной опознавательный признак — по ней карточка читается быстрее,
  // чем по подписи.
  lv_obj_t *box = lv_obj_create(par);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_size(box, w, h);
  const int r = std::min(h, w) / 2 > 34 ? 34 : std::min(h, w) / 2;
  lv_obj_set_style_radius(box, r, LV_PART_MAIN);
  lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(box, lv_color_hex(card_bg()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  // Плавный переход цвета при смене состояния. Резкий скачок читается
  // как мигание и заставляет глаз возвращаться к карточке.
  static lv_style_transition_dsc_t tr;
  static const lv_style_prop_t tr_props[] = {LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_PROP_INV};
  lv_style_transition_dsc_init(&tr, tr_props, lv_anim_path_ease_out, 260, 0, nullptr);
  lv_obj_set_style_transition(box, &tr, LV_PART_MAIN);

  // Заливка по уровню: у света ширина полосы показывает яркость прямо
  // на карточке, как в Bubble Card. Лежит под содержимым.
  lv_obj_t *fill = lv_obj_create(box);
  lv_obj_remove_style_all(fill);
  lv_obj_set_pos(fill, 0, 0);
  lv_obj_set_size(fill, 0, h);
  lv_obj_set_style_radius(fill, r, LV_PART_MAIN);
  lv_obj_set_style_bg_color(fill, accent, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(fill, LV_OPA_30, LV_PART_MAIN);
  lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);

  // Круг с символом слева.
  const int d = std::min(h - 24, 72);
  lv_obj_t *ibox = lv_obj_create(box);
  lv_obj_remove_style_all(ibox);
  lv_obj_set_size(ibox, d, d);
  lv_obj_align(ibox, LV_ALIGN_LEFT_MID, 14, 0);
  lv_obj_set_style_radius(ibox, d / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ibox, accent, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ibox, LV_OPA_20, LV_PART_MAIN);

  lv_obj_add_flag(ibox, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ibox, icon_event_cb, LV_EVENT_CLICKED, card);

  lv_obj_t *icon = lv_label_create(ibox);
  lv_label_set_text(icon, icon_for(card->type, card->icon_name));
  lv_obj_center(icon);
  lv_obj_set_style_text_color(icon, accent, LV_PART_MAIN);
  if (this->font_icon_ != nullptr)
    lv_obj_set_style_text_font(icon, static_cast<const lv_font_t *>(this->font_icon_), LV_PART_MAIN);

  const int tx = 14 + d + 14;

  lv_obj_t *name = lv_label_create(box);
  lv_label_set_text(name, card->label.c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, w - tx - 14);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, tx, -14);
  lv_obj_set_style_text_color(name, lv_color_hex(card_ink2()), LV_PART_MAIN);
  if (this->font_small_ != nullptr)
    lv_obj_set_style_text_font(name, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);

  lv_obj_t *value = lv_label_create(box);
  lv_label_set_text(value, "—");
  lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
  lv_obj_set_width(value, w - tx - 14);
  lv_obj_align(value, LV_ALIGN_LEFT_MID, tx, 14);
  lv_obj_set_style_text_color(value, lv_color_hex(card_ink()), LV_PART_MAIN);
  if (this->font_value_ != nullptr)
    lv_obj_set_style_text_font(value, static_cast<const lv_font_t *>(this->font_value_), LV_PART_MAIN);

  // Третья строка нужна не всегда — прячем, пока нечего показать.
  lv_obj_t *sub = lv_label_create(box);
  lv_label_set_text(sub, "");
  lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, tx, -10);
  lv_obj_set_style_text_color(sub, lv_color_hex(card_ink3()), LV_PART_MAIN);
  if (this->font_small_ != nullptr)
    lv_obj_set_style_text_font(sub, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);
  lv_obj_add_flag(sub, LV_OBJ_FLAG_HIDDEN);

  card->box = box;
  card->fill = fill;
  card->icon = icon;
  card->icon_box = ibox;
  card->lbl_name = name;
  card->lbl_value = value;
  card->lbl_sub = sub;
  card->owner = this;

  const bool controllable =
      card->type == "light" || card->type == "switch" || card->type == "valve" ||
      card->type == "cover" || card->type == "scene" || card->type == "script" || card->type == "lock";
  if (controllable) {
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, card_event_cb, LV_EVENT_CLICKED, card);
  } else {
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
  }
}

// Цвет карточки: если лампа сообщила свой RGB и горит — берём его.
// Иначе цвет по типу. Так карточка тёплого света выглядит тёплой,
// а холодного — холодной, как в образце.
static lv_color_t card_color(const PanelUI::Card *card) {
  if (card->rgb >= 0 && card->active)
    return lv_color_hex(static_cast<uint32_t>(card->rgb));
  return accent_for(card->type);
}

void PanelUI::refresh_card_colors(Card *card) {
  const lv_color_t c = card_color(card);
  if (card->icon_box != nullptr) {
    auto *ib = static_cast<lv_obj_t *>(card->icon_box);
    lv_obj_set_style_bg_color(ib, c, LV_PART_MAIN);
  }
  if (card->icon != nullptr && !card->active)
    lv_obj_set_style_text_color(static_cast<lv_obj_t *>(card->icon), c, LV_PART_MAIN);
  if (card->fill != nullptr)
    lv_obj_set_style_bg_color(static_cast<lv_obj_t *>(card->fill), c, LV_PART_MAIN);
  if (card->box != nullptr && card->active)
    lv_obj_set_style_bg_color(static_cast<lv_obj_t *>(card->box), c, LV_PART_MAIN);
}

void PanelUI::update_card_level_(Card *card, int level) {
  card->level = level;
  auto *fill = static_cast<lv_obj_t *>(card->fill);
  auto *box = static_cast<lv_obj_t *>(card->box);
  if (fill == nullptr || box == nullptr)
    return;
  if (level <= 0 || !card->active) {
    lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const int w = lv_obj_get_width(box);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);

  // Плавно: скачок ширины на каждом обновлении яркости выглядит дёрганым,
  // особенно когда яркость меняют ползунком и значения идут потоком.
  const int target = w * level / 100;
  const int now = lv_obj_get_width(fill);
  if (now == target)
    return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, fill);
  lv_anim_set_values(&a, now, target);
  lv_anim_set_time(&a, 240);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
    lv_obj_set_width(static_cast<lv_obj_t *>(o), v);
  });
  lv_anim_start(&a);
}

// Человеческое представление состояния: «on» на настенной панели читается
// хуже, чем «Вкл».
static std::string humanize(const std::string &state) {
  if (state == "on")          return "Вкл";
  if (state == "off")         return "Выкл";
  if (state == "open")        return "Открыто";
  if (state == "closed")      return "Закрыто";
  if (state == "locked")      return "Заперто";
  if (state == "unlocked")    return "Открыт";
  if (state == "playing")     return "Играет";
  if (state == "paused")      return "Пауза";
  if (state == "idle")        return "Ожидание";
  if (state == "heat")        return "Нагрев";
  if (state == "cool")        return "Охлаждение";
  if (state == "auto")        return "Авто";
  if (state == "unavailable") return "нет связи";
  if (state == "unknown")     return "—";
  return state;
}

void PanelUI::apply_state_(Card *card, const std::string &state) {
  auto *value = static_cast<lv_obj_t *>(card->lbl_value);
  auto *box = static_cast<lv_obj_t *>(card->box);
  if (value == nullptr)
    return;

  std::string shown = humanize(state);

  bool numeric = false;
  float num = NAN;
  {
    char *end = nullptr;
    num = strtof(state.c_str(), &end);
    numeric = (end != state.c_str() && end != nullptr && *end == '\0');
  }
  if (numeric) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", card->decimals, num);
    shown = buf;
    if (!card->unit.empty()) {
      shown += " ";
      shown += card->unit;
    }
  }
  lv_label_set_text(value, shown.c_str());

  // Включённое подсвечивается целиком: сам пузырь, круг и символ.
  // Состояние должно читаться с двух метров, а не по мелкой надписи.
  if (box != nullptr) {
    const bool active = (state == "on" || state == "open" || state == "heat" ||
                         state == "cool" || state == "playing" || state == "unlocked");
    card->active = active;
    const lv_color_t acc = card_color(card);
    lv_obj_set_style_bg_color(box, active ? acc : lv_color_hex(card_bg()), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, active ? LV_OPA_20 : LV_OPA_COVER, LV_PART_MAIN);
    if (card->icon_box != nullptr) {
      auto *ib = static_cast<lv_obj_t *>(card->icon_box);
      lv_obj_set_style_bg_color(ib, acc, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(ib, active ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
    }
    if (card->icon != nullptr) {
      lv_obj_set_style_text_color(static_cast<lv_obj_t *>(card->icon),
                                  active ? lv_color_hex(card_bg_page()) : acc, LV_PART_MAIN);
    }
    this->update_card_level_(card, card->level);
  }

  // Пороги: выход за границу окрашивает значение тревожным цветом.
  if (numeric) {
    const bool warn = (!std::isnan(card->warn_above) && num > card->warn_above) ||
                      (!std::isnan(card->warn_below) && num < card->warn_below);
    lv_obj_set_style_text_color(value, warn ? lv_color_hex(0xC24038) : lv_color_hex(card_ink()),
                                LV_PART_MAIN);
  }
}

void PanelUI::update_card_value_(Card *card, const std::string &state) {
  if (card->lbl_value == nullptr)
    return;

  // Подавление дребезга. Измерено на живом HA: датчик напряжения шлёт
  // обновление раз в 1,6 с, и 88 % изменений меньше 0,5 В — это шум АЦП.
  // Без подавления карточка перерисовывается вечно, экран не засыпает
  // и панель греется в закрытом подрозетнике. См. docs/02-ha-survey.md.
  char *end = nullptr;
  const float num = strtof(state.c_str(), &end);
  const bool numeric = (end != state.c_str() && end != nullptr && *end == '\0');

  const uint32_t now = millis();
  if (numeric && !std::isnan(card->last_num)) {
    if (card->deadband > 0.0f && std::fabs(num - card->last_num) < card->deadband)
      return;
    if (card->throttle_ms > 0 && (now - card->last_draw) < card->throttle_ms)
      return;
  }
  card->last_num = numeric ? num : NAN;
  card->last_draw = now;

  ESP_LOGD(TAG, "карточка '%s' (%s) <- %s", card->label.c_str(), card->entity.c_str(), state.c_str());
  this->apply_state_(card, state);
}

void PanelUI::sync_dots() {
  if (this->dots_.empty() || this->scroller_ == nullptr)
    return;
  auto *sc = static_cast<lv_obj_t *>(this->scroller_);
  const int w = lv_obj_get_width(sc);
  if (w <= 0)
    return;
  const int x = lv_obj_get_scroll_x(sc);
  size_t active = (size_t) ((x + w / 2) / w);
  if (active >= this->dots_.size())
    active = this->dots_.size() - 1;
  for (size_t i = 0; i < this->dots_.size(); i++) {
    lv_obj_set_style_bg_color(static_cast<lv_obj_t *>(this->dots_[i]),
                              lv_color_hex(i == active ? g_accent : 0x3A4753), LV_PART_MAIN);
  }
}

void PanelUI::demo_fill() {
  // Правдоподобные значения по типам: позволяет проверить вёрстку
  // и поведение карточек, когда Home Assistant недоступен.
  int i = 0;
  for (auto *card : this->cards_) {
    std::string fake;
    if (card->type == "light" || card->type == "switch" || card->type == "valve")
      fake = (i % 2 == 0) ? "on" : "off";
    else if (card->type == "climate")
      fake = "heat";
    else if (card->type == "cover")
      fake = (i % 2 == 0) ? "open" : "closed";
    else
      fake = (i % 3 == 0) ? "23.4" : (i % 3 == 1 ? "231.7" : "95.5");
    card->last_num = NAN;
    card->last_draw = 0;
    this->apply_state_(card, fake);
    i++;
  }
  ESP_LOGI(TAG, "демо-режим: заполнено карточек %u", (unsigned) this->cards_.size());
}

void PanelUI::render_page_(void *tile, const void *page_json, int w, int h) {
  const JsonObject &page = *static_cast<const JsonObject *>(page_json);

  int cols = 2, rows = 4;
  JsonArray grid = page["grid"].as<JsonArray>();
  if (!grid.isNull() && grid.size() == 2) {
    cols = grid[0].as<int>();
    rows = grid[1].as<int>();
  }
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;

  const int gap = 12, pad = 12, header = 46;
  auto *par = static_cast<lv_obj_t *>(tile);

  // Заголовок страницы: без него при нескольких страницах непонятно, где ты.
  const char *title = page["title"] | "";
  if (*title != 0) {
    lv_obj_t *hdr = lv_label_create(par);
    lv_label_set_text(hdr, title);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_color(hdr, lv_color_hex(card_ink()), LV_PART_MAIN);
    if (this->font_title_ != nullptr)
      lv_obj_set_style_text_font(hdr, static_cast<const lv_font_t *>(this->font_title_), LV_PART_MAIN);
  }

  const int top = (*title != 0) ? header : pad;
  const int cw = (w - 2 * pad - (cols - 1) * gap) / cols;
  const int ch = (h - top - pad - (rows - 1) * gap) / rows;

  // Карта занятости: карточка может занимать несколько клеток, поэтому
  // класть их подряд по индексу уже нельзя — ищем первое место, куда
  // помещается. Порядок в раскладке сохраняется, дырки заполняются
  // следующими карточками, если те мельче.
  std::vector<bool> busy(static_cast<size_t>(cols) * rows, false);
  auto fits = [&](int cx, int cy, int w, int h) {
    if (cx + w > cols || cy + h > rows)
      return false;
    for (int y = cy; y < cy + h; y++)
      for (int x = cx; x < cx + w; x++)
        if (busy[static_cast<size_t>(y) * cols + x])
          return false;
    return true;
  };

  for (JsonObject jc : page["cards"].as<JsonArray>()) {
    auto *card = new Card();  // NOLINT
    card->type = jc["type"] | "";
    card->entity = jc["entity"] | "";
    card->label = jc["label"] | "";
    card->unit = jc["unit"] | "";
    card->icon_name = std::string(jc["icon"] | "");
    card->decimals = jc["decimals"] | 1;
    if (card->label.empty())
      card->label = card->entity.empty() ? card->type : card->entity;
    JsonArray sp = jc["span"].as<JsonArray>();
    if (!sp.isNull() && sp.size() == 2) {
      card->span_w = sp[0].as<int>();
      card->span_h = sp[1].as<int>();
    }
    if (card->span_w < 1) card->span_w = 1;
    if (card->span_h < 1) card->span_h = 1;
    if (card->span_w > cols) card->span_w = cols;
    if (card->span_h > rows) card->span_h = rows;

    for (JsonObject jb : jc["buttons"].as<JsonArray>()) {
      RowButton rb;
      rb.entity = std::string(jb["entity"] | "");
      rb.label = std::string(jb["label"] | "");
      if (rb.label.empty())
        rb.label = rb.entity;
      if (!rb.entity.empty())
        card->buttons.push_back(rb);
    }

    card->deadband = jc["deadband"] | this->def_deadband_;
    card->throttle_ms = parse_ms(jc["throttle"] | this->def_throttle_.c_str());
    card->warn_above = jc["warn_above"] | NAN;
    card->warn_below = jc["warn_below"] | NAN;

    int gx = -1, gy = -1;
    for (int y = 0; y < rows && gy < 0; y++)
      for (int x = 0; x < cols; x++)
        if (fits(x, y, card->span_w, card->span_h)) { gx = x; gy = y; break; }

    if (gx < 0) {
      ESP_LOGW(TAG, "на странице '%s' не поместилась карточка '%s' (%dx%d клеток)", title,
               card->label.c_str(), card->span_w, card->span_h);
      delete card;
      continue;
    }
    for (int y = gy; y < gy + card->span_h; y++)
      for (int x = gx; x < gx + card->span_w; x++)
        busy[static_cast<size_t>(y) * cols + x] = true;

    const int px = pad + gx * (cw + gap);
    const int py = top + gy * (ch + gap);
    const int pw = card->span_w * cw + (card->span_w - 1) * gap;
    const int ph = card->span_h * ch + (card->span_h - 1) * gap;
    this->render_card_(par, card, px, py, pw, ph);

    // Появление: карточка выезжает снизу и проявляется, с задержкой
    // по порядку. Только при первом построении — при заливке раскладки
    // из редактора экран должен обновиться сразу, а не переигрывать
    // появление на каждое сохранение.
    if (this->animate_build_) {
      auto *bx = static_cast<lv_obj_t *>(card->box);
      const uint32_t delay = 40 * static_cast<uint32_t>(this->cards_.size());
      lv_obj_set_style_opa(bx, LV_OPA_TRANSP, LV_PART_MAIN);

      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, bx);
      lv_anim_set_values(&a, py + 26, py);
      lv_anim_set_time(&a, 320);
      lv_anim_set_delay(&a, delay);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
        lv_obj_set_y(static_cast<lv_obj_t *>(o), v);
      });
      lv_anim_start(&a);

      lv_anim_t f;
      lv_anim_init(&f);
      lv_anim_set_var(&f, bx);
      lv_anim_set_values(&f, LV_OPA_TRANSP, LV_OPA_COVER);
      lv_anim_set_time(&f, 300);
      lv_anim_set_delay(&f, delay);
      lv_anim_set_exec_cb(&f, [](void *o, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(o), static_cast<lv_opa_t>(v), LV_PART_MAIN);
      });
      lv_anim_start(&f);
    }

    this->cards_.push_back(card);
  }
}

bool PanelUI::build_ui(void *root) {
  if (root == nullptr) {
    ESP_LOGE(TAG, "build_ui: контейнер не задан");
    return false;
  }
  const std::string data = this->read_layout();
  if (data.empty()) {
    ESP_LOGW(TAG, "build_ui: раскладки нет");
    return false;
  }

  for (auto *c : this->cards_)
    delete c;
  this->cards_.clear();
  this->dots_.clear();
  this->scroller_ = nullptr;

  auto *par = static_cast<lv_obj_t *>(root);
  // Снимаем все анимации ДО удаления объектов: иначе анимация продолжает
  // жить на удалённом объекте, а новая карточка остаётся прозрачной —
  // выглядит так, будто карточки пропали с экрана.
  lv_anim_delete_all();
  lv_obj_clean(par);

  // Размеры надо запрашивать ПОСЛЕ пересчёта раскладки: на on_boot LVGL
  // ещё не считал геометрию, и get_width вернёт 0. Тогда карточки выходят
  // нулевого размера, и на экране остаётся только фон контейнера — белый.
  lv_obj_update_layout(par);
  int rw = lv_obj_get_width(par);
  int rh = lv_obj_get_height(par);
  if (rw <= 1 || rh <= 1) {
    lv_display_t *disp = lv_display_get_default();
    rw = disp != nullptr ? lv_display_get_horizontal_resolution(disp) : 720;
    rh = disp != nullptr ? lv_display_get_vertical_resolution(disp) : 720;
    ESP_LOGW(TAG, "размер контейнера не посчитан, беру разрешение экрана: %dx%d", rw, rh);
  }
  ESP_LOGI(TAG, "рисую в области %dx%d", rw, rh);

  // Свой фон: у lv_obj по умолчанию светлая заливка, и на ней ничего не видно.
  g_dark = this->s_theme_ != "light";
  lv_obj_set_style_bg_color(par, lv_color_hex(card_bg_page()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(par, LV_OPA_COVER, LV_PART_MAIN);

  size_t n_pages = 0;
  bool ok = json::parse_json(data, [&](JsonObject doc) -> bool {
    // Тема
    JsonObject theme = doc["theme"].as<JsonObject>();
    if (!theme.isNull()) {
      const char *acc = theme["accent"] | "";
      if (acc[0] == '#' && strlen(acc) == 7)
        g_accent = strtoul(acc + 1, nullptr, 16);
      this->theme_radius_ = theme["radius"] | 16;
    }
    JsonObject defs = doc["defaults"].as<JsonObject>();
    this->def_deadband_ = defs.isNull() ? 0.0f : (defs["deadband"] | 0.0f);
    this->def_throttle_ = defs.isNull() ? "1s" : std::string(defs["throttle"] | "1s");

    JsonArray pages = doc["pages"].as<JsonArray>();
    if (pages.isNull() || pages.size() == 0) {
      ESP_LOGE(TAG, "в раскладке нет страниц");
      return false;
    }

    // Свайп между страницами: горизонтальная прокрутка с привязкой к центру.
    // Специально НЕ tileview: тот виджет ESPHome собирает, только если он
    // объявлен в YAML, а это снова связало бы рантайм с пересборкой.
    // Здесь достаточно обычного lv_obj, который уже есть всегда.
    lv_obj_t *scroller = lv_obj_create(par);
    lv_obj_set_size(scroller, rw, rh);
    lv_obj_set_style_bg_color(scroller, lv_color_hex(card_bg_page()), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scroller, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroller, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scroller, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(scroller, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(scroller, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(scroller, LV_SCROLLBAR_MODE_OFF);
    this->scroller_ = scroller;

    for (JsonObject page : pages) {
      if (page["hidden"] | false)
        continue;
      lv_obj_t *tile = lv_obj_create(scroller);
      lv_obj_set_size(tile, rw, rh);
      // Абсолютное позиционирование: LV_USE_FLEX в сборке ESPHome выключен.
      lv_obj_set_pos(tile, (int) n_pages * rw, 0);
      lv_obj_set_style_bg_color(tile, lv_color_hex(card_bg_page()), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
      lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
      this->render_page_(tile, &page, rw, rh);
      n_pages++;
      if (n_pages >= 8) {
        ESP_LOGW(TAG, "больше 8 страниц не строим");
        break;
      }
    }
    // Точки-индикатор: со свайпом без них непонятно, где ты и сколько
    // страниц всего. Рисуем поверх прокрутки, чтобы не уезжали вместе с ней.
    if (n_pages > 1) {
      const int dot = 8, dgap = 10;
      const int total = (int) n_pages * dot + ((int) n_pages - 1) * dgap;
      int dx = (rw - total) / 2;
      for (size_t i = 0; i < n_pages; i++) {
        lv_obj_t *d = lv_obj_create(par);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_pos(d, dx, rh - 18);
        lv_obj_set_style_radius(d, dot / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(d, lv_color_hex(i == 0 ? g_accent : 0x3A4753), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
        this->dots_.push_back(d);
        dx += dot + dgap;
      }
      // Подсветка активной точки по прокрутке.
      lv_obj_add_event_cb(scroller, scroll_event_cb, LV_EVENT_SCROLL_END, this);
    }

    return n_pages > 0;
  });

  if (!ok) {
    ESP_LOGE(TAG, "раскладка не разобрана");
    return false;
  }
  // Появление показываем один раз, при первом построении. Дальше правки
  // из редактора применяются мгновенно.
  this->animate_build_ = false;
  ESP_LOGI(TAG, "построено страниц: %u, карточек: %u", (unsigned) n_pages, (unsigned) this->cards_.size());
  return true;
}

void PanelUI::bind_entities() {
#ifdef USE_API
  size_t n = 0;
  for (auto *card : this->cards_) {
    if (card->entity.empty())
      continue;
    api::global_api_server->subscribe_home_assistant_state(
        card->entity, optional<std::string>(), std::function<void(StringRef)>([this, card](StringRef state) {
          this->update_card_value_(card, std::string(state.c_str(), state.size()));
        }));
    n++;

    // Яркость света — отдельной подпиской на атрибут. Именно она даёт
    // заливку карточки, как в Bubble Card.
    if (card->type == "light") {
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("brightness"),
          std::function<void(StringRef)>([this, card](StringRef v) {
            const int raw = atoi(std::string(v.c_str(), v.size()).c_str());
            this->update_card_level_(card, raw > 0 ? (raw * 100 + 127) / 255 : 0);
          }));
      n++;
    }
    // Цвет лампы: карточка окрашивается в настоящий цвет света,
    // как в образце. HA присылает атрибут строкой вида "[255, 180, 100]".
    if (card->type == "light") {
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("rgb_color"),
          std::function<void(StringRef)>([this, card](StringRef v) {
            std::string t(v.c_str(), v.size());
            int r = -1, g = -1, b = -1;
            if (sscanf(t.c_str(), "[%d, %d, %d]", &r, &g, &b) == 3 && r >= 0)
              card->rgb = (r << 16) | (g << 8) | b;
            else
              card->rgb = -1;
            this->refresh_card_colors(card);
          }));
      n++;
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("color_temp_kelvin"),
          std::function<void(StringRef)>([card](StringRef v) {
            card->color_temp = atoi(std::string(v.c_str(), v.size()).c_str());
          }));
      n++;
    }

    // Климат: и текущая температура на карточке, и уставка для подробностей.
    if (card->type == "climate") {
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("current_temperature"),
          std::function<void(StringRef)>([card](StringRef v) {
            std::string t(v.c_str(), v.size());
            card->current_temp = strtof(t.c_str(), nullptr);
            if (card->lbl_sub == nullptr) return;
            lv_label_set_text(static_cast<lv_obj_t *>(card->lbl_sub), ("сейчас " + t + "°").c_str());
            lv_obj_remove_flag(static_cast<lv_obj_t *>(card->lbl_sub), LV_OBJ_FLAG_HIDDEN);
          }));
      n++;
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("temperature"),
          std::function<void(StringRef)>([card](StringRef v) {
            card->target_temp = strtof(std::string(v.c_str(), v.size()).c_str(), nullptr);
          }));
      n++;
    }

    if (card->type == "cover") {
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("current_position"),
          std::function<void(StringRef)>([card](StringRef v) {
            card->position = atoi(std::string(v.c_str(), v.size()).c_str());
          }));
      n++;
    }
    if (card->type == "media") {
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("volume_level"),
          std::function<void(StringRef)>([card](StringRef v) {
            const float f = strtof(std::string(v.c_str(), v.size()).c_str(), nullptr);
            card->volume = static_cast<int>(f * 100.0f + 0.5f);
          }));
      n++;
    }
    if (card->type == "fan") {
      api::global_api_server->subscribe_home_assistant_state(
          card->entity, optional<std::string>("percentage"),
          std::function<void(StringRef)>([card](StringRef v) {
            card->fan_pct = atoi(std::string(v.c_str(), v.size()).c_str());
          }));
      n++;
    }
  }
  ESP_LOGI(TAG, "подписок оформлено: %u. Состояния придут после переподключения к API", (unsigned) n);
#else
  ESP_LOGW(TAG, "API выключен — привязка невозможна");
#endif
}

void PanelUI::call_service_for(const std::string &entity, const std::string &service) {
#ifdef USE_API
  api::HomeassistantActionRequest req;
  req.service = StringRef(service);
  req.data.init(1);
  api::HomeassistantServiceMap kv;
  kv.key = StringRef("entity_id");
  kv.value = StringRef(entity);
  req.data.push_back(kv);
  ESP_LOGI(TAG, "действие: %s %s", service.c_str(), entity.c_str());
  api::global_api_server->send_homeassistant_action(req);
#endif
}

void PanelUI::call_service(const std::string &service, const char *key, const std::string &value) {
#ifdef USE_API
  if (this->sheet_card_ == nullptr && key != nullptr)
    return;
  api::HomeassistantActionRequest req;
  req.service = StringRef(service);
  const std::string ent = this->sheet_card_ != nullptr ? this->sheet_card_->entity : std::string();
  req.data.init(key != nullptr ? 2 : 1);
  api::HomeassistantServiceMap kv;
  kv.key = StringRef("entity_id");
  kv.value = StringRef(ent);
  req.data.push_back(kv);
  if (key != nullptr) {
    api::HomeassistantServiceMap kv2;
    kv2.key = StringRef(key);
    kv2.value = StringRef(value);
    req.data.push_back(kv2);
  }
  ESP_LOGI(TAG, "действие: %s %s%s%s", service.c_str(), ent.c_str(), key ? " " : "",
           key ? value.c_str() : "");
  api::global_api_server->send_homeassistant_action(req);
#endif
}

static void sheet_close_cb(lv_event_t *e) {
  auto *self = static_cast<PanelUI *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->close_details();
}

void PanelUI::close_details() {
  if (this->sheet_ == nullptr)
    return;
  lv_obj_delete(static_cast<lv_obj_t *>(this->sheet_));
  this->sheet_ = nullptr;
  this->sheet_card_ = nullptr;
}

void PanelUI::open_details(Card *card) {
  this->close_details();
  if (card == nullptr || card->entity.empty())
    return;
  this->sheet_card_ = card;

  lv_obj_t *top = lv_layer_top();
  lv_display_t *disp = lv_display_get_default();
  const int W = disp ? lv_display_get_horizontal_resolution(disp) : 720;
  const int H = disp ? lv_display_get_vertical_resolution(disp) : 720;

  // Затемнение под панелью: касание мимо закрывает. Так подробности
  // не превращаются в ловушку, из которой не выйти.
  lv_obj_t *scrim = lv_obj_create(top);
  lv_obj_remove_style_all(scrim);
  lv_obj_set_size(scrim, W, H);
  lv_obj_set_pos(scrim, 0, 0);
  lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scrim, LV_OPA_60, LV_PART_MAIN);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scrim, sheet_close_cb, LV_EVENT_CLICKED, this);
  this->sheet_ = scrim;

  const int SH = 440;
  lv_obj_t *sheet = lv_obj_create(scrim);
  lv_obj_set_size(sheet, W, SH);
  lv_obj_set_pos(sheet, 0, H - SH);
  lv_obj_set_style_radius(sheet, 34, LV_PART_MAIN);
  lv_obj_set_style_border_width(sheet, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sheet, lv_color_hex(card_bg()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(sheet, 24, LV_PART_MAIN);
  lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

  const lv_color_t acc = accent_for(card->type);

  lv_obj_t *ib = lv_obj_create(sheet);
  lv_obj_remove_style_all(ib);
  lv_obj_set_size(ib, 76, 76);
  lv_obj_align(ib, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_radius(ib, 38, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ib, acc, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ib, card->active ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
  lv_obj_t *ic = lv_label_create(ib);
  lv_label_set_text(ic, icon_for(card->type, card->icon_name));
  lv_obj_center(ic);
  lv_obj_set_style_text_color(ic, card->active ? lv_color_hex(card_bg_page()) : acc, LV_PART_MAIN);
  if (this->font_icon_ != nullptr)
    lv_obj_set_style_text_font(ic, static_cast<const lv_font_t *>(this->font_icon_), LV_PART_MAIN);

  lv_obj_t *ttl = lv_label_create(sheet);
  lv_label_set_text(ttl, card->label.c_str());
  lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ttl, W - 48 - 100);
  lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 100, 8);
  lv_obj_set_style_text_color(ttl, lv_color_hex(card_ink()), LV_PART_MAIN);
  if (this->font_title_ != nullptr)
    lv_obj_set_style_text_font(ttl, static_cast<const lv_font_t *>(this->font_title_), LV_PART_MAIN);

  lv_obj_t *ent = lv_label_create(sheet);
  lv_label_set_text(ent, card->entity.c_str());
  lv_label_set_long_mode(ent, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ent, W - 48 - 100);
  lv_obj_align(ent, LV_ALIGN_TOP_LEFT, 100, 48);
  lv_obj_set_style_text_color(ent, lv_color_hex(card_ink3()), LV_PART_MAIN);
  if (this->font_small_ != nullptr)
    lv_obj_set_style_text_font(ent, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);

  // Кнопка закрытия — крупная: у стены в неё надо попадать не глядя.
  lv_obj_t *cl = lv_button_create(sheet);
  lv_obj_set_size(cl, 96, 62);
  lv_obj_align(cl, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_radius(cl, 31, LV_PART_MAIN);
  lv_obj_add_event_cb(cl, sheet_close_cb, LV_EVENT_CLICKED, this);
  lv_obj_t *cll = lv_label_create(cl);
  lv_label_set_text(cll, "Закрыть");
  lv_obj_center(cll);
  if (this->font_small_ != nullptr)
    lv_obj_set_style_text_font(cll, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);

  this->build_details_controls(sheet, card, W - 48);
}

static lv_obj_t *sheet_button(lv_obj_t *parent, PanelUI *self, const char *text, int x, int y, int w,
                              int h, const std::string &service, const char *key, const std::string &value,
                              const void *font) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  if (font != nullptr)
    lv_obj_set_style_text_font(l, static_cast<const lv_font_t *>(font), LV_PART_MAIN);

  auto *a = new SheetAction{self, service, key != nullptr ? key : "", value};  // NOLINT
  lv_obj_add_event_cb(b, sheet_action_cb, LV_EVENT_CLICKED, a);
  lv_obj_add_event_cb(b, sheet_action_free_cb, LV_EVENT_DELETE, a);
  return b;
}

static void sheet_slider_cb(lv_event_t *e) {
  auto *a = static_cast<SheetAction *>(lv_event_get_user_data(e));
  auto *sl = static_cast<lv_obj_t *>(lv_event_get_target(e));
  if (a == nullptr || sl == nullptr)
    return;
  const int v = static_cast<int>(lv_slider_get_value(sl));
  // Громкость Home Assistant принимает долей от нуля до единицы,
  // а не процентами — ползунок при этом удобнее в процентах.
  if (a->key == "volume_level") {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", v / 100.0f);
    a->self->call_service(a->service, a->key.c_str(), buf);
    return;
  }
  a->self->call_service(a->service, a->key.c_str(), std::to_string(v));
}

void PanelUI::build_details_controls(void *sheet_v, Card *card, int width) {
  auto *sheet = static_cast<lv_obj_t *>(sheet_v);
  const int Y0 = 120;
  const int BH = 78;
  const std::string dom = card->entity.substr(0, card->entity.find('.'));
  const lv_color_t acc = accent_for(card->type);

  // Заголовок раздела внутри подробностей.
  auto caption = [&](const char *text, int y) {
    lv_obj_t *c = lv_label_create(sheet);
    lv_label_set_text(c, text);
    lv_obj_set_pos(c, 0, y);
    lv_obj_set_style_text_color(c, lv_color_hex(card_ink2()), LV_PART_MAIN);
    if (this->font_small_ != nullptr)
      lv_obj_set_style_text_font(c, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);
  };

  auto slider = [&](int y, int lo, int hi, int val, const char *service, const char *key) {
    lv_obj_t *sl = lv_slider_create(sheet);
    lv_obj_set_size(sl, width, 46);
    lv_obj_set_pos(sl, 0, y);
    lv_slider_set_range(sl, lo, hi);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_radius(sl, 23, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 23, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, acc, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, acc, LV_PART_KNOB);
    auto *a2 = new SheetAction{this, service, key, ""};  // NOLINT
    lv_obj_add_event_cb(sl, sheet_slider_cb, LV_EVENT_RELEASED, a2);
    lv_obj_add_event_cb(sl, sheet_action_free_cb, LV_EVENT_DELETE, a2);
    return sl;
  };

  // Ряд кнопок в одну строку, ширина делится поровну.
  auto row = [&](int y, std::initializer_list<std::pair<const char *, std::array<std::string, 3>>> items) {
    const int n = static_cast<int>(items.size());
    const int bw = (width - 16 * (n - 1)) / n;
    int i = 0;
    for (const auto &it : items) {
      const auto &a3 = it.second;
      sheet_button(sheet, this, it.first, i * (bw + 16), y, bw, BH, a3[0],
                   a3[1].empty() ? nullptr : a3[1].c_str(), a3[2], this->font_value_);
      i++;
    }
  };

  if (card->type == "light") {
    caption("Яркость", Y0);
    slider(Y0 + 34, 1, 100, card->level > 0 ? card->level : 50, "light.turn_on", "brightness_pct");
    caption("Оттенок белого", Y0 + 104);
    slider(Y0 + 138, 2000, 6500, card->color_temp > 0 ? card->color_temp : 3000, "light.turn_on",
           "kelvin");
    row(Y0 + 208, {{"Включить", {"light.turn_on", "", ""}},
                   {"Выключить", {"light.turn_off", "", ""}}});

  } else if (card->type == "switch" || card->type == "valve") {
    row(Y0, {{"Включить", {dom + ".turn_on", "", ""}},
             {"Выключить", {dom + ".turn_off", "", ""}}});
    caption("Нажатие на карточку переключает состояние", Y0 + BH + 20);

  } else if (card->type == "lock") {
    row(Y0, {{"Отпереть", {"lock.unlock", "", ""}},
             {"Запереть", {"lock.lock", "", ""}}});

  } else if (card->type == "cover") {
    row(Y0, {{"Вверх", {"cover.open_cover", "", ""}},
             {"Стоп", {"cover.stop_cover", "", ""}},
             {"Вниз", {"cover.close_cover", "", ""}}});
    caption("Положение", Y0 + BH + 20);
    slider(Y0 + BH + 54, 0, 100, card->position >= 0 ? card->position : 50,
           "cover.set_cover_position", "position");

  } else if (card->type == "climate") {
    // Показываем и уставку, и текущую: без этого непонятно, что вообще
    // происходит — греет ли и до чего.
    {
      char cap[64];
      if (!std::isnan(card->current_temp))
        snprintf(cap, sizeof(cap), "Уставка, °C   ·   сейчас %.1f°", card->current_temp);
      else
        snprintf(cap, sizeof(cap), "Уставка, °C");
      caption(cap, Y0);
    }
    slider(Y0 + 34, 5, 35,
           !std::isnan(card->target_temp) ? static_cast<int>(card->target_temp + 0.5f) : 22,
           "climate.set_temperature", "temperature");
    caption("Режим", Y0 + 104);
    row(Y0 + 138, {{"Нагрев", {"climate.set_hvac_mode", "hvac_mode", "heat"}},
                   {"Холод", {"climate.set_hvac_mode", "hvac_mode", "cool"}},
                   {"Авто", {"climate.set_hvac_mode", "hvac_mode", "auto"}}});
    row(Y0 + 138 + BH + 16, {{"Выключить", {"climate.set_hvac_mode", "hvac_mode", "off"}}});

  } else if (card->type == "media") {
    row(Y0, {{"Назад", {"media_player.media_previous_track", "", ""}},
             {"Пуск и пауза", {"media_player.media_play_pause", "", ""}},
             {"Вперёд", {"media_player.media_next_track", "", ""}}});
    caption("Громкость", Y0 + BH + 20);
    slider(Y0 + BH + 54, 0, 100, card->volume >= 0 ? card->volume : 30, "media_player.volume_set",
           "volume_level");
    row(Y0 + BH + 124, {{"Выключить", {"media_player.turn_off", "", ""}}});

  } else if (card->type == "fan") {
    caption("Скорость", Y0);
    slider(Y0 + 34, 0, 100, card->fan_pct >= 0 ? card->fan_pct : 50, "fan.set_percentage",
           "percentage");
    row(Y0 + 104, {{"Включить", {"fan.turn_on", "", ""}},
                   {"Выключить", {"fan.turn_off", "", ""}}});

  } else if (card->type == "scene" || card->type == "script") {
    row(Y0, {{"Запустить", {dom + ".turn_on", "", ""}}});
    caption("Сцена срабатывает сразу, подтверждения нет", Y0 + BH + 20);

  } else if (card->type == "camera") {
    caption("Изображение с камеры на панели пока не показывается", Y0);
    caption("Карточка отражает доступность камеры", Y0 + 34);

  } else {
    // Датчик: крупное значение и подпись. Управлять нечем, но заглянуть
    // ради цифры — самая частая причина сюда зайти.
    lv_obj_t *big = lv_label_create(sheet);
    lv_label_set_text(big, card->lbl_value != nullptr
                               ? lv_label_get_text(static_cast<lv_obj_t *>(card->lbl_value))
                               : "—");
    lv_obj_set_pos(big, 0, Y0 + 20);
    lv_obj_set_style_text_color(big, lv_color_hex(card_ink()), LV_PART_MAIN);
    if (this->font_title_ != nullptr)
      lv_obj_set_style_text_font(big, static_cast<const lv_font_t *>(this->font_title_), LV_PART_MAIN);
    if (!card->unit.empty())
      caption(("единицы: " + card->unit).c_str(), Y0 + 90);
  }
}

void PanelUI::on_card_tapped(Card *card) {
#ifdef USE_API
  if (card->entity.empty())
    return;
  const std::string domain = card->entity.substr(0, card->entity.find('.'));
  const std::string service = domain + ".toggle";

  api::HomeassistantActionRequest req;
  req.service = StringRef(service);
  req.data.init(1);
  api::HomeassistantServiceMap kv;
  kv.key = StringRef("entity_id");
  kv.value = StringRef(card->entity);
  req.data.push_back(kv);

  ESP_LOGI(TAG, "касание: %s -> %s", card->entity.c_str(), service.c_str());
  api::global_api_server->send_homeassistant_action(req);
#endif
}

// ---------------------------------------------------------------------------
// Управляющий канал: заливка раскладки по HTTP.
//
// Это то, с чем будет говорить веб-редактор. Отдельный сервер, а не
// web_server ESPHome: тому нельзя добавить свои маршруты.
//
// GET  /layout.json — отдать текущую раскладку
// POST /layout.json — сохранить новую и сразу перестроить экран
// ---------------------------------------------------------------------------

bool PanelUI::rebuild() {
  if (this->root_ == nullptr) {
    ESP_LOGW(TAG, "перестроение невозможно: контейнер не задан");
    return false;
  }
  if (!this->build_ui(this->root_))
    return false;
  this->bind_entities();
  return true;
}

// Редактор открывается с другого источника (файл на диске или другой хост),
// поэтому без заголовков CORS браузер запрос не выпустит.
static void add_cors(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

// Предварительный запрос браузера перед POST с JSON.
// Посредник к Home Assistant.
//
// Браузер к HA напрямую не пустят: предварительный запрос отвергается
// с 403, заголовков CORS у HA по умолчанию нет. Поэтому список сущностей
// забирает панель — ей ограничения браузера не писаны.
//
// Берём не /api/states (574 КиБ JSON, который панели пришлось бы разбирать),
// а /api/template: HA сам собирает компактный список строкой, и панели
// остаётся передать её дальше как есть. На реальной установке из 1271
// сущности это 48 КиБ вместо 574.
//
// Токен НЕ сохраняется: он приходит с каждым запросом от редактора,
// используется один раз и забывается. Долгоживущему токену на стене
// не место (см. ADR-0002).
static esp_err_t handle_ha_entities(httpd_req_t *req) {
  add_cors(req);

  if (req->content_len == 0 || req->content_len > 4096) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"error\":\"нет данных для подключения\"}");
  }
  std::string body;
  body.reserve(req->content_len);
  char rb[512];
  size_t left = req->content_len;
  while (left > 0) {
    int got = httpd_req_recv(req, rb, std::min(left, sizeof(rb)));
    if (got <= 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_sendstr(req, "{\"error\":\"обрыв приёма\"}");
    }
    body.append(rb, got);
    left -= got;
  }

  std::string ha_url, token;
  json::parse_json(body, [&](JsonObject o) -> bool {
    ha_url = std::string(o["url"] | "");
    token = std::string(o["token"] | "");
    return true;
  });
  auto *self = static_cast<PanelUI *>(req->user_ctx);
  while (!ha_url.empty() && ha_url.back() == '/')
    ha_url.pop_back();

  // Если редактор ничего не прислал — берём сохранённое на панели.
  // Так список сущностей работает и после смены браузера, и с телефона.
  if (ha_url.empty())
    ha_url = self->ha_url();
  if (token.empty())
    token = self->ha_token();

  if (ha_url.empty() || token.empty()) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"error\":\"нужны адрес и токен\"}");
  }

  static const char *const TEMPLATE_BODY =
      "{\"template\":\"{% for s in states if s.domain in ["
      "'light','switch','climate','cover','valve','sensor','binary_sensor',"
      "'media_player','lock','scene','script','fan','camera','number','button'] %}"
      "{{s.entity_id}}\\t{{s.name}}\\t{{area_name(s.entity_id) or ''}}\\n{% endfor %}\"}";

  // Ставим сервер имён прямо здесь. Ethernet, объявленный без кабеля,
  // повторяет DHCP каждые 15 секунд и каждый раз затирает общие настройки
  // имён — фоновой проверки раз в полминуты не хватает, запрос попадает
  // в окно без DNS.
  {
    const ip_addr_t *cur = dns_getserver(0);
    if (cur == nullptr || ip_addr_isany(cur)) {
      ip_addr_t fb;
      IP_ADDR4(&fb, 1, 1, 1, 1);
      dns_setserver(0, &fb);
      IP_ADDR4(&fb, 8, 8, 8, 8);
      dns_setserver(1, &fb);
      ESP_LOGW(TAG, "перед запросом к HA не было DNS — поставил резервный");
    }
  }

  const std::string url = ha_url + "/api/template";
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 20000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;

  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (cli == nullptr) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"error\":\"не удалось создать клиента\"}");
  }
  const std::string auth = "Bearer " + token;
  esp_http_client_set_header(cli, "Authorization", auth.c_str());
  esp_http_client_set_header(cli, "Content-Type", "application/json");
  esp_http_client_set_post_field(cli, TEMPLATE_BODY, strlen(TEMPLATE_BODY));

  esp_err_t err = esp_http_client_open(cli, strlen(TEMPLATE_BODY));
  if (err != ESP_OK) {
    esp_http_client_cleanup(cli);
    ESP_LOGW(TAG, "HA недоступен: %s", esp_err_to_name(err));
    httpd_resp_set_status(req, "502 Bad Gateway");
    return httpd_resp_sendstr(req, "{\"error\":\"панель не достучалась до Home Assistant\"}");
  }
  esp_http_client_write(cli, TEMPLATE_BODY, strlen(TEMPLATE_BODY));
  esp_http_client_fetch_headers(cli);
  const int status = esp_http_client_get_status_code(cli);

  if (status != 200) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    ESP_LOGW(TAG, "HA ответил %d", status);
    httpd_resp_set_status(req, status == 401 ? "401 Unauthorized" : "502 Bad Gateway");
    return httpd_resp_sendstr(req, status == 401 ? "{\"error\":\"токен не принят\"}"
                                                 : "{\"error\":\"Home Assistant ответил ошибкой\"}");
  }

  // Отдаём потоком: список бывает в десятки килобайт, держать его целиком
  // в памяти незачем.
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  char chunk[1024];
  int n, total = 0;
  while ((n = esp_http_client_read(cli, chunk, sizeof(chunk))) > 0) {
    if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK)
      break;
    total += n;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);
  // Сработало — запоминаем, чтобы не спрашивать снова.
  self->save_ha(ha_url, token);
  ESP_LOGI(TAG, "список сущностей от HA: %d байт", total);
  return ESP_OK;
}

// Редактор отдаём с самой панели: иначе им неудобно пользоваться —
// файл пришлось бы держать на диске и вручную вписывать в него адрес.
// Редактор спрашивает, помнит ли панель подключение к HA. Токен наружу
// не отдаём никогда — только адрес и признак наличия.
static esp_err_t handle_get_ha(httpd_req_t *req) {
  auto *self = static_cast<PanelUI *>(req->user_ctx);
  add_cors(req);
  httpd_resp_set_type(req, "application/json");
  const std::string url = self->ha_url();
  const bool has = !self->ha_token().empty();
  std::string out = "{\"url\":\"" + url + "\",\"saved\":" + (has ? "true" : "false") + "}";
  return httpd_resp_sendstr(req, out.c_str());
}

static esp_err_t handle_get_editor(httpd_req_t *req) {
  add_cors(req);
  // Без этого браузер держит старую страницу после обновления прошивки,
  // и человек правит интерфейс, которого уже нет.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, EDITOR_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_get_settings(httpd_req_t *req) {
  auto *self = static_cast<PanelUI *>(req->user_ctx);
  add_cors(req);
  httpd_resp_set_type(req, "application/json");
  const std::string data = self->read_settings();
  if (data.empty()) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "{\"error\":\"настроек нет\"}");
  }
  return httpd_resp_send(req, data.c_str(), data.size());
}

static esp_err_t handle_post_settings(httpd_req_t *req) {
  auto *self = static_cast<PanelUI *>(req->user_ctx);
  add_cors(req);

  static const size_t MAX_SETTINGS = 8 * 1024;
  if (req->content_len == 0 || req->content_len > MAX_SETTINGS) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    return httpd_resp_sendstr(req, "{\"error\":\"пустые или слишком большие настройки\"}");
  }

  std::string body;
  body.reserve(req->content_len);
  char buf[512];
  size_t left = req->content_len;
  while (left > 0) {
    int got = httpd_req_recv(req, buf, std::min(left, sizeof(buf)));
    if (got <= 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_sendstr(req, "{\"error\":\"обрыв приёма\"}");
    }
    body.append(buf, got);
    left -= got;
  }

  // Проверяем до записи: испорченный JSON не должен затирать рабочие
  // настройки и оставить панель, например, с нулевой яркостью.
  bool valid = json::parse_json(body, [](JsonObject doc) -> bool {
    return !doc["display"].isNull() || !doc["time"].isNull();
  });
  if (!valid) {
    httpd_resp_set_status(req, "422 Unprocessable Entity");
    return httpd_resp_sendstr(req, "{\"error\":\"не разбирается или нет display/time\"}");
  }

  if (!self->write_settings(body)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"error\":\"не удалось сохранить\"}");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"saved\":true}");
}

static esp_err_t handle_options(httpd_req_t *req) {
  add_cors(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t handle_get_layout(httpd_req_t *req) {
  auto *self = static_cast<PanelUI *>(req->user_ctx);
  add_cors(req);
  const std::string data = self->read_layout();
  httpd_resp_set_type(req, "application/json");
  if (data.empty()) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "{\"error\":\"раскладки нет\"}");
  }
  return httpd_resp_send(req, data.c_str(), data.size());
}

static esp_err_t handle_post_layout(httpd_req_t *req) {
  auto *self = static_cast<PanelUI *>(req->user_ctx);
  add_cors(req);

  // Верхняя граница нужна: без неё большой запрос съест память панели.
  static const size_t MAX_LAYOUT = 64 * 1024;
  if (req->content_len == 0 || req->content_len > MAX_LAYOUT) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    return httpd_resp_sendstr(req, "{\"error\":\"пустая или слишком большая раскладка\"}");
  }

  std::string body;
  body.reserve(req->content_len);
  char buf[1024];
  size_t left = req->content_len;
  while (left > 0) {
    int got = httpd_req_recv(req, buf, std::min(left, sizeof(buf)));
    if (got <= 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_sendstr(req, "{\"error\":\"обрыв приёма\"}");
    }
    body.append(buf, got);
    left -= got;
  }

  // Сначала проверяем, что это вообще разбирается, и только потом пишем:
  // испорченный JSON не должен затереть рабочую раскладку.
  bool valid = json::parse_json(body, [](JsonObject doc) -> bool {
    return !doc["pages"].as<JsonArray>().isNull();
  });
  if (!valid) {
    ESP_LOGW(TAG, "отклонена нераспознанная раскладка (%u байт)", (unsigned) body.size());
    httpd_resp_set_status(req, "422 Unprocessable Entity");
    return httpd_resp_sendstr(req, "{\"error\":\"не разбирается или нет pages\"}");
  }

  if (!self->write_layout(body)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"error\":\"не удалось сохранить\"}");
  }

  // Перестроение НЕ здесь: мы в задаче веб-сервера, а LVGL не потокобезопасен.
  // Ставим отметку, главный цикл подхватит её в своём такте.
  self->request_rebuild();

  httpd_resp_set_type(req, "application/json");
  char out[128];
  snprintf(out, sizeof(out), "{\"saved\":%u,\"rebuilt\":\"queued\"}", (unsigned) body.size());
  return httpd_resp_sendstr(req, out);
}

void PanelUI::start_http_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->http_port_;
  cfg.ctrl_port = this->http_port_ + 1000;  // иначе конфликт с web_server ESPHome
  cfg.lru_purge_enable = true;
  cfg.max_uri_handlers = 16;
  cfg.stack_size = 20480;  // TLS-рукопожатие в 8 КиБ не помещается

  httpd_handle_t server = nullptr;
  esp_err_t err = httpd_start(&server, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP-сервер не запустился на порту %u: %s", this->http_port_, esp_err_to_name(err));
    return;
  }
  this->httpd_ = server;

  httpd_uri_t get_uri = {};
  get_uri.uri = "/layout.json";
  get_uri.method = HTTP_GET;
  get_uri.handler = handle_get_layout;
  get_uri.user_ctx = this;
  httpd_register_uri_handler(server, &get_uri);

  httpd_uri_t hag_uri = {};
  hag_uri.uri = "/ha/config";
  hag_uri.method = HTTP_GET;
  hag_uri.handler = handle_get_ha;
  hag_uri.user_ctx = this;
  httpd_register_uri_handler(server, &hag_uri);

  httpd_uri_t ha_uri = {};
  ha_uri.uri = "/ha/entities";
  ha_uri.method = HTTP_POST;
  ha_uri.handler = handle_ha_entities;
  ha_uri.user_ctx = this;
  httpd_register_uri_handler(server, &ha_uri);

  httpd_uri_t haopt_uri = {};
  haopt_uri.uri = "/ha/entities";
  haopt_uri.method = HTTP_OPTIONS;
  haopt_uri.handler = handle_options;
  haopt_uri.user_ctx = this;
  httpd_register_uri_handler(server, &haopt_uri);

  httpd_uri_t root_uri = {};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = handle_get_editor;
  root_uri.user_ctx = this;
  httpd_register_uri_handler(server, &root_uri);

  httpd_uri_t set_uri = {};
  set_uri.uri = "/settings.json";
  set_uri.method = HTTP_GET;
  set_uri.handler = handle_get_settings;
  set_uri.user_ctx = this;
  httpd_register_uri_handler(server, &set_uri);

  httpd_uri_t setpost_uri = {};
  setpost_uri.uri = "/settings.json";
  setpost_uri.method = HTTP_POST;
  setpost_uri.handler = handle_post_settings;
  setpost_uri.user_ctx = this;
  httpd_register_uri_handler(server, &setpost_uri);

  httpd_uri_t setopt_uri = {};
  setopt_uri.uri = "/settings.json";
  setopt_uri.method = HTTP_OPTIONS;
  setopt_uri.handler = handle_options;
  setopt_uri.user_ctx = this;
  httpd_register_uri_handler(server, &setopt_uri);

  httpd_uri_t opt_uri = {};
  opt_uri.uri = "/layout.json";
  opt_uri.method = HTTP_OPTIONS;
  opt_uri.handler = handle_options;
  opt_uri.user_ctx = this;
  httpd_register_uri_handler(server, &opt_uri);

  httpd_uri_t post_uri = {};
  post_uri.uri = "/layout.json";
  post_uri.method = HTTP_POST;
  post_uri.handler = handle_post_layout;
  post_uri.user_ctx = this;
  httpd_register_uri_handler(server, &post_uri);

  ESP_LOGI(TAG, "веб-интерфейс: http://<адрес панели>:%u/", this->http_port_);
}

}  // namespace panel_ui
}  // namespace esphome
