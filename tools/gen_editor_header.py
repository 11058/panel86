#!/usr/bin/env python3
"""Превратить editor/index.html в заголовок для прошивки.

Панель отдаёт редактор сама, по своему адресу — иначе им неудобно
пользоваться: файл на диске надо где-то держать и вручную вписывать
в него адрес панели.

Запускать после правки редактора:
    python3 tools/gen_editor_header.py
"""
import sys

SRC = "editor/index.html"
DST = "firmware/editor_html.h"
DELIM = "PANELEDITOR"

html = open(SRC, encoding="utf-8").read()
if f'){DELIM}"' in html:
    sys.exit("разделитель встречается в файле — выберите другой")

raw = html.encode("utf-8")
with open(DST, "w", encoding="utf-8") as f:
    f.write("// Сгенерировано tools/gen_editor_header.py из editor/index.html.\n")
    f.write("// Не править вручную: правки затрёт следующая генерация.\n")
    f.write("#pragma once\n\n")
    f.write(f"static const char EDITOR_HTML[] = R\"{DELIM}(\n")
    f.write(html)
    f.write(f"\n){DELIM}\";\n")
print(f"{DST}: {len(raw)} байт")
