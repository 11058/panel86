#!/usr/bin/env python3
"""Собрать список страниц LVGL из конфигов и вписать его в theme-fix.yaml.

Красить страницы по списку надёжнее, чем из on_load: on_load срабатывает,
когда страница уже отрисована, и первый показ выходит белым. Но список,
написанный руками, отстаёт от жизни — поэтому он генерируется.

Запускать после добавления новой страницы:
    python3 tools/gen_page_list.py
"""
import glob
import re
import sys

FILES = ["firmware/panel86-p0.yaml"] + sorted(glob.glob("firmware/packages/*.yaml"))
MARK_A = "          // НАЧАЛО СПИСКА СТРАНИЦ (генерируется tools/gen_page_list.py)\n"
MARK_B = "          // КОНЕЦ СПИСКА СТРАНИЦ\n"

pages = []
for f in FILES:
    for m in re.finditer(r"^\s*- id: (page_[a-z0-9_]+)$", open(f).read(), re.M):
        if m.group(1) not in pages:
            pages.append(m.group(1))

if not pages:
    sys.exit("страниц не найдено — проверьте формат конфигов")

body = "          lv_obj_t *const pages[] = {\n"
for i in range(0, len(pages), 3):
    body += "            " + ", ".join("id(%s)->obj" % p for p in pages[i:i+3]) + ",\n"
body += "          };\n"

path = "firmware/packages/theme-fix.yaml"
s = open(path).read()
a, b = s.index(MARK_A), s.index(MARK_B)
open(path, "w").write(s[:a] + MARK_A + body + s[b:])
print("страниц в списке: %d" % len(pages))
print("  " + ", ".join(pages))
