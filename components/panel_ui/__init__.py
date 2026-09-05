"""panel_ui — хранилище и интерпретатор раскладки интерфейса.

Первый этап P3: файловая система. Раскладка должна лежать на панели
как ДАННЫЕ и меняться без пересборки прошивки — это вся суть проекта,
см. docs/adr/0001-esphome-kak-fundament.md.

В ESPHome нет ни LittleFS, ни SPIFFS, поэтому LittleFS подтягивается
как управляемый компонент ESP-IDF из реестра.

ESPHome по умолчанию вырезает операции с каталогами из VFS ради экономии
флеша, и тогда remove() и rename() возвращают ENOSYS (88) — атомарное
сохранение раскладки через временный файл становится невозможным.
Компонент объявляет свою потребность сам, через require_vfs_dir().
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import add_idf_component, require_vfs_dir
from esphome.const import CONF_ID

CODEOWNERS = ["@11058"]
DEPENDENCIES = ["esp32", "json", "network"]
AUTO_LOAD = ["json"]

panel_ui_ns = cg.esphome_ns.namespace("panel_ui")
PanelUI = panel_ui_ns.class_("PanelUI", cg.Component)

CONF_PARTITION = "partition"
CONF_BASE_PATH = "base_path"
CONF_LAYOUT_FILE = "layout_file"
CONF_HTTP_PORT = "http_port"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PanelUI),
        # Метка раздела из firmware/partitions.csv
        cv.Optional(CONF_PARTITION, default="storage"): cv.string_strict,
        cv.Optional(CONF_BASE_PATH, default="/fs"): cv.string_strict,
        cv.Optional(CONF_LAYOUT_FILE, default="layout.json"): cv.string_strict,
        # 8080, а не 80: web_server ESPHome занимает 80.
        cv.Optional(CONF_HTTP_PORT, default=8080): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    add_idf_component(name="joltwallet/littlefs", ref="1.16.4")
    # Без этого remove()/rename() на LittleFS возвращают ENOSYS.
    require_vfs_dir()

    # Рантайм-часть API Home Assistant существует в прошивке только за этими
    # ifdef-ами, а ESPHome включает их лишь при КОМПИЛИРУЕМОМ использовании:
    # подписке через platform: homeassistant или вызове homeassistant.action.
    # Нам нужно и то, и другое в рантайме, поэтому объявляем сами —
    # так пользователю компонента не нужен ни фиктивный «якорь», ни лишний YAML.
    cg.add_define("USE_API_HOMEASSISTANT_STATES")
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_partition(config[CONF_PARTITION]))
    cg.add(var.set_base_path(config[CONF_BASE_PATH]))
    cg.add(var.set_layout_file(config[CONF_LAYOUT_FILE]))
    cg.add(var.set_http_port(config[CONF_HTTP_PORT]))
