#!/usr/bin/env python3
"""Разведка Home Assistant по WebSocket.

Отвечает на три вопроса, от которых зависит архитектура панели:
  1. Сколько весит снапшот subscribe_entities целиком и с фильтром.
  2. Какой поток обновлений в простое.
  3. Как заполнен реестр областей — без него room-first UI не имеет смысла.

Запуск:
    export HA_WS="wss://хост/api/websocket"
    export HA_TOKEN="долгоживущий токен"     # НЕ коммитить
    export HA_CA="/путь/к/roots.pem"         # см. README, macOS
    python3 tools/ha_probe.py

Токен читается только из окружения и никуда не записывается.
"""
import asyncio, collections, json, os, ssl, time
import websockets

URL = os.environ["HA_WS"]
TOK = os.environ["HA_TOKEN"]
CTX = ssl.create_default_context(cafile=os.environ["HA_CA"]) if os.environ.get("HA_CA") else None

CONTROLLABLE = ("light.", "switch.", "climate.", "cover.", "fan.",
                "valve.", "lock.", "media_player.", "scene.")
WINDOW = int(os.environ.get("HA_WINDOW", "30"))  # секунд наблюдения за потоком


async def call(ws, st, payload):
    st["id"] += 1
    await ws.send(json.dumps(dict(payload, id=st["id"])))
    while True:
        m = json.loads(await ws.recv())
        if m.get("id") == st["id"] and m.get("type") == "result":
            return m


async def snapshot(ws, st, entity_ids=None):
    """Подписаться и вернуть (байт, сущностей, секунд, id подписки)."""
    st["id"] += 1
    sid = st["id"]
    req = {"id": sid, "type": "subscribe_entities"}
    if entity_ids:
        req["entity_ids"] = entity_ids
    t0 = time.monotonic()
    await ws.send(json.dumps(req))
    while True:
        raw = await ws.recv()
        m = json.loads(raw)
        if m.get("id") == sid and m.get("type") == "event":
            return len(raw), len(m["event"].get("a", {})), time.monotonic() - t0, sid


async def main():
    async with websockets.connect(URL, max_size=32 << 20, ping_interval=20, ssl=CTX) as ws:
        hello = json.loads(await ws.recv())
        await ws.send(json.dumps({"type": "auth", "access_token": TOK}))
        auth = json.loads(await ws.recv())
        if auth.get("type") != "auth_ok":
            print("авторизация не прошла:", auth)
            return
        print(f"подключено: Home Assistant {hello.get('ha_version')}\n")

        st = {"id": 0}
        areas = (await call(ws, st, {"type": "config/area_registry/list"}))["result"]
        ents  = (await call(ws, st, {"type": "config/entity_registry/list"}))["result"]
        devs  = (await call(ws, st, {"type": "config/device_registry/list"}))["result"]

        dev_area = {d["id"]: d.get("area_id") for d in devs}
        names = {a["area_id"]: a["name"] for a in areas}
        rooms = collections.defaultdict(list)
        orphans = 0
        visible = 0
        for e in ents:
            if e.get("hidden_by") or e.get("disabled_by"):
                continue
            visible += 1
            aid = e.get("area_id") or dev_area.get(e.get("device_id"))
            if aid:
                rooms[aid].append(e["entity_id"])
            else:
                orphans += 1

        print(f"областей {len(areas)} · устройств {len(devs)} · в реестре {len(ents)}"
              f" (скрытых и отключённых {len(ents) - visible})")
        print(f"видимых {visible}, из них без области: {orphans} "
              f"({orphans * 100 // max(visible, 1)} %)"
              f"  <- room-first UI держится на этом числе\n")

        print(f"{'Комната':<20}{'всего':>7}{'управляемых':>13}")
        for aid, lst in sorted(rooms.items(), key=lambda kv: -len(kv[1])):
            ctl = [x for x in lst if x.startswith(CONTROLLABLE)]
            print(f"{names.get(aid, aid):<20}{len(lst):>7}{len(ctl):>13}")

        # --- замеры подписки ---
        full = await snapshot(ws, st)
        print(f"\nsubscribe_entities без фильтра: {full[0] / 1024:.0f} КиБ, "
              f"{full[1]} сущностей, {full[2]:.2f} с")

        await call(ws, st, {"type": "unsubscribe_events", "subscription": full[3]})

        biggest = max(rooms.values(), key=len) if rooms else []
        if biggest:
            filt = await snapshot(ws, st, biggest)
            print(f"subscribe_entities с фильтром: {filt[0]} Б, "
                  f"{filt[1]} сущностей, {filt[2]:.2f} с")
            if full[0] and filt[0]:
                print(f"выигрыш: в {full[0] / filt[0]:.0f} раз · "
                      f"{filt[0] // max(filt[1], 1)} Б на сущность")
            print("фильтр", "РАБОТАЕТ" if filt[1] <= len(biggest) else "ИГНОРИРУЕТСЯ")

        # --- поток в простое ---
        t1 = time.monotonic()
        total = msgs = 0
        touched = collections.Counter()
        while (left := WINDOW - (time.monotonic() - t1)) > 0:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=left)
            except asyncio.TimeoutError:
                break
            m = json.loads(raw)
            if m.get("type") == "event":
                total += len(raw)
                msgs += 1
                touched.update(m["event"].get("c", {}).keys())
        print(f"\nпоток по фильтрованной подписке за {WINDOW} с: {msgs} сообщений, {total / 1024:.1f} КиБ "
              f"({total / WINDOW:.0f} Б/с), затронуто {len(touched)} сущностей")
        if touched:
            print("говорливые:", ", ".join(f"{k} ({v})" for k, v in touched.most_common(6)))


if __name__ == "__main__":
    asyncio.run(main())
