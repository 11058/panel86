#include "panel_ui.h"

#include "esphome/core/log.h"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>

#include "esp_littlefs.h"

namespace esphome {
namespace panel_ui {

static const char *const TAG = "panel_ui";

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

}  // namespace panel_ui
}  // namespace esphome
