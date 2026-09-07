#include "panel_ui.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>

#include "esp_littlefs.h"
#include "esp_http_server.h"

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

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LittleFS не смонтирован на разделе '%s': %s", this->partition_, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->mounted_ = true;

  size_t used = 0, total = 0;
  if (this->fs_usage(&used, &total)) {
    ESP_LOGI(TAG, "LittleFS смонтирован: %u из %u КиБ занято", (unsigned) (used / 1024), (unsigned) (total / 1024));
  }

  std::string layout = this->read_layout();
  if (layout.empty()) {
    ESP_LOGW(TAG, "Раскладки нет — панель не настроена");
  } else {
    ESP_LOGI(TAG, "Раскладка найдена: %u байт", (unsigned) layout.size());
  }
}

void PanelUI::loop() {
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
// Интерпретатор: JSON -> дерево LVGL
// ---------------------------------------------------------------------------

// Цвет акцента по типу карточки. Пока грубо: смысл в том, чтобы типы
// различались на экране, а не в красоте — тема появится позже.
// Оттенок по типу. Акцент темы задаёт управляемые элементы, измеряемые
// остаются нейтральными — так на экране видно, что можно трогать.
static uint32_t g_accent = 0xC2610C;

static lv_color_t accent_for(const std::string &type) {
  if (type == "light" || type == "switch" || type == "scene" || type == "script")
    return lv_color_hex(g_accent);
  if (type == "climate") return lv_color_hex(0xA83232);
  if (type == "valve" || type == "cover") return lv_color_hex(0x2E6DA4);
  return lv_color_hex(0x5A6875);
}

static void card_event_cb(lv_event_t *e) {
  auto *card = static_cast<PanelUI::Card *>(lv_event_get_user_data(e));
  if (card != nullptr && card->owner != nullptr)
    card->owner->on_card_tapped(card);
}

void PanelUI::render_card_(void *parent, Card *card, int x, int y, int w, int h) {
  auto *par = static_cast<lv_obj_t *>(parent);
  const lv_color_t accent = accent_for(card->type);

  lv_obj_t *box = lv_obj_create(par);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_size(box, w, h);
  lv_obj_set_style_radius(box, this->theme_radius_, LV_PART_MAIN);
  lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, accent, LV_PART_MAIN);
  lv_obj_set_style_pad_all(box, 14, LV_PART_MAIN);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x161C22), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  // Название — приглушённое, сверху.
  lv_obj_t *name = lv_label_create(box);
  lv_label_set_text(name, card->label.c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, w - 32);
  lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(0x9AA8B4), LV_PART_MAIN);
  if (this->font_small_ != nullptr)
    lv_obj_set_style_text_font(name, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);

  // Главное значение — крупное, цветное.
  lv_obj_t *value = lv_label_create(box);
  lv_label_set_text(value, "—");
  lv_obj_align(value, LV_ALIGN_LEFT_MID, 0, 6);
  lv_obj_set_style_text_color(value, accent, LV_PART_MAIN);
  if (this->font_title_ != nullptr)
    lv_obj_set_style_text_font(value, static_cast<const lv_font_t *>(this->font_title_), LV_PART_MAIN);

  // Вторая строка: уставка у климата, единицы у датчика, режим у остальных.
  lv_obj_t *sub = lv_label_create(box);
  lv_label_set_text(sub, "");
  lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x6B7B88), LV_PART_MAIN);
  if (this->font_small_ != nullptr)
    lv_obj_set_style_text_font(sub, static_cast<const lv_font_t *>(this->font_small_), LV_PART_MAIN);

  card->box = box;
  card->lbl_name = name;
  card->lbl_value = value;
  card->lbl_sub = sub;
  card->owner = this;

  // Управляемое реагирует на касание, измеряемое — нет.
  // Пользователь должен видеть разницу до того, как ткнёт.
  const bool controllable =
      card->type == "light" || card->type == "switch" || card->type == "valve" ||
      card->type == "cover" || card->type == "scene" || card->type == "script" || card->type == "lock";
  if (controllable) {
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, card_event_cb, LV_EVENT_CLICKED, card);
    lv_label_set_text(sub, "нажмите, чтобы переключить");
  } else {
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    if (!card->unit.empty())
      lv_label_set_text(sub, card->unit.c_str());
  }
}

// Человеческое представление состояния. HA присылает строки, и «on»
// на настенной панели читается хуже, чем «Вкл».
static std::string humanize(const std::string &state) {
  if (state == "on")          return "Вкл";
  if (state == "off")         return "Выкл";
  if (state == "open")        return "Открыто";
  if (state == "closed")      return "Закрыто";
  if (state == "unavailable") return "нет связи";
  if (state == "unknown")     return "—";
  if (state == "heat")        return "Нагрев";
  if (state == "cool")        return "Охлаждение";
  if (state == "idle")        return "Ожидание";
  if (state == "auto")        return "Авто";
  return state;
}

void PanelUI::apply_state_(Card *card, const std::string &state) {
  auto *value = static_cast<lv_obj_t *>(card->lbl_value);
  auto *box = static_cast<lv_obj_t *>(card->box);
  if (value == nullptr)
    return;

  std::string shown = humanize(state);

  // Числовые типы: округляем до заданной точности и дописываем единицы.
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

  // Включённое подсвечиваем фоном: состояние должно читаться с двух метров,
  // а не по мелкой надписи.
  if (box != nullptr) {
    const bool active = (state == "on" || state == "open" || state == "heat");
    lv_obj_set_style_bg_color(box, active ? accent_for(card->type) : lv_color_hex(0x161C22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, active ? LV_OPA_30 : LV_OPA_COVER, LV_PART_MAIN);
  }

  // Пороги: выход за границу окрашивает значение тревожным цветом.
  if (numeric) {
    bool warn = (!std::isnan(card->warn_above) && num > card->warn_above) ||
                (!std::isnan(card->warn_below) && num < card->warn_below);
    lv_obj_set_style_text_color(value, warn ? lv_color_hex(0xA32217) : accent_for(card->type), LV_PART_MAIN);
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
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xE3E9EE), LV_PART_MAIN);
    if (this->font_title_ != nullptr)
      lv_obj_set_style_text_font(hdr, static_cast<const lv_font_t *>(this->font_title_), LV_PART_MAIN);
  }

  const int top = (*title != 0) ? header : pad;
  const int cw = (w - 2 * pad - (cols - 1) * gap) / cols;
  const int ch = (h - top - pad - (rows - 1) * gap) / rows;

  int idx = 0;
  for (JsonObject jc : page["cards"].as<JsonArray>()) {
    if (idx >= cols * rows) {
      ESP_LOGW(TAG, "на странице '%s' карточек больше, чем клеток %dx%d — лишние пропущены", title, cols, rows);
      break;
    }
    auto *card = new Card();  // NOLINT
    card->type = jc["type"] | "";
    card->entity = jc["entity"] | "";
    card->label = jc["label"] | "";
    card->unit = jc["unit"] | "";
    card->decimals = jc["decimals"] | 1;
    if (card->label.empty())
      card->label = card->entity.empty() ? card->type : card->entity;
    card->deadband = jc["deadband"] | this->def_deadband_;
    card->throttle_ms = parse_ms(jc["throttle"] | this->def_throttle_.c_str());
    card->warn_above = jc["warn_above"] | NAN;
    card->warn_below = jc["warn_below"] | NAN;

    const int cx = pad + (idx % cols) * (cw + gap);
    const int cy = top + (idx / cols) * (ch + gap);
    this->render_card_(par, card, cx, cy, cw, ch);
    this->cards_.push_back(card);
    idx++;
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

  auto *par = static_cast<lv_obj_t *>(root);
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
  lv_obj_set_style_bg_color(par, lv_color_hex(0x0B0F13), LV_PART_MAIN);
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
    lv_obj_set_style_bg_color(scroller, lv_color_hex(0x0B0F13), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scroller, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroller, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scroller, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(scroller, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(scroller, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(scroller, LV_SCROLLBAR_MODE_OFF);

    for (JsonObject page : pages) {
      if (page["hidden"] | false)
        continue;
      lv_obj_t *tile = lv_obj_create(scroller);
      lv_obj_set_size(tile, rw, rh);
      // Абсолютное позиционирование: LV_USE_FLEX в сборке ESPHome выключен.
      lv_obj_set_pos(tile, (int) n_pages * rw, 0);
      lv_obj_set_style_bg_color(tile, lv_color_hex(0x0B0F13), LV_PART_MAIN);
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
    return n_pages > 0;
  });

  if (!ok) {
    ESP_LOGE(TAG, "раскладка не разобрана");
    return false;
  }
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
  }
  ESP_LOGI(TAG, "подписок оформлено: %u. Состояния придут после переподключения к API", (unsigned) n);
#else
  ESP_LOGW(TAG, "API выключен — привязка невозможна");
#endif
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

  const bool built = self->rebuild();
  httpd_resp_set_type(req, "application/json");
  char out[128];
  snprintf(out, sizeof(out), "{\"saved\":%u,\"cards\":%u,\"rebuilt\":%s}", (unsigned) body.size(),
           (unsigned) self->card_count(), built ? "true" : "false");
  return httpd_resp_sendstr(req, out);
}

void PanelUI::start_http_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->http_port_;
  cfg.ctrl_port = this->http_port_ + 1000;  // иначе конфликт с web_server ESPHome
  cfg.lru_purge_enable = true;
  cfg.max_uri_handlers = 6;
  cfg.stack_size = 8192;

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

  ESP_LOGI(TAG, "приём раскладки: http://<панель>:%u/layout.json", this->http_port_);
}

}  // namespace panel_ui
}  // namespace esphome
