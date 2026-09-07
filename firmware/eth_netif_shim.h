// Заголовки для настройки Ethernet напрямую через esp_netif.
//
// У компонента ESPHome метод set_manual_ip защищённый, поэтому статический
// адрес задаётся по стандартному ключу интерфейса ETH_DEF.
#pragma once
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
