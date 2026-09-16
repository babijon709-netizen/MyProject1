#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Собирает jni/src/lang_tables.inc — таблицы перевода РУ/EN (см. lang.cpp).

    python3 tools/lang/gen_tables.py        # перезаписать lang_tables.inc
    python3 tools/lang/check.py             # проверить, что файл не разошёлся

Русские строки — источник перевода: строка из XS("...") в main.cpp и подписи
визуалов из game.cpp. Здесь же лежат их английские варианты. Подписи оружия не
пишутся руками: они берутся из таблицы самой игры (kWeaponNames в game.cpp,
колонка `en` — то имя, которое игра пишет в своём UI), поэтому в оверлее оно
такое же, как в игре. Для подписи, которой игра даёт несколько имён (например
«Топор» — это и Axe, и Hatchet), выбор закреплён в kWeaponEnPick.

Скрипт заодно проверяет полноту: если в main.cpp появилась строка с кириллицей
без перевода — он не запишет файл, а скажет, какую строку добавить.
"""

import io
import os
import re
import sys

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
MAIN = os.path.join(REPO, 'jni/src/main.cpp')
GAME = os.path.join(REPO, 'jni/src/game.cpp')
OUT = os.path.join(REPO, 'jni/src/lang_tables.inc')


def strip_comments(src):
    """Убрать комментарии, сохранив строковые литералы (в них есть // и *)."""
    out, i, n = [], 0, len(src)
    in_quote, in_comm = False, None
    while i < n:
        c = src[i]
        if in_quote:
            if c == '\\':
                out.append(src[i:i + 2]); i += 2; continue
            if c == '"':
                in_quote = False
            out.append(c); i += 1; continue
        if in_comm == 'line':
            if c == '\n':
                in_comm = None; out.append(c)
            i += 1; continue
        if in_comm == 'block':
            if src.startswith('*/', i):
                in_comm = None; i += 2; continue
            if c == '\n':
                out.append(c)
            i += 1; continue
        if src.startswith('//', i):
            in_comm = 'line'; i += 2; continue
        if src.startswith('/*', i):
            in_comm = 'block'; i += 2; continue
        if c == '"':
            in_quote = True; out.append(c); i += 1; continue
        out.append(c); i += 1
    return ''.join(out)


def has_cyr(s):
    return re.search(r'[А-Яа-яЁё]', s) is not None


def strip_log_calls(src):
    """Убрать аргументы вызовов мини-лога (mlog::line / mlog::every) и меток фазы.

    Их строки не переводятся: лог разбирают по русским подписям на устройстве,
    как и прежние строки лога автофарма. Без этого правила любая запись в лог или
    подпись фазы чтения требовала бы английского перевода и валила бы сборку."""
    out, i, n = [], 0, len(src)
    needles = ('mlog::line(', 'mlog::every(', 'set_phase(')
    while i < n:
        needle = next((nd for nd in needles if src.startswith(nd, i)), None)
        if needle is None:
            out.append(src[i]); i += 1; continue
        out.append(' ' * len(needle)); i += len(needle)
        depth = 1
        while i < n and depth:
            c = src[i]
            if c == '"':
                out.append(' '); i += 1
                while i < n and src[i] != '"':
                    if src[i] == '\\':
                        out.append('  '); i += 2; continue
                    out.append(' '); i += 1
                if i < n:
                    out.append(' '); i += 1
                continue
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            out.append(' ' if depth else ')')
            i += 1
    return ''.join(out)


# ---------------------------------------------------------------------------
# Интерфейс: русская строка (как написана в коде) -> английская.
# Форматные строки переводятся вместе с единицами: "%.0f м" -> "%.0f m".
# ---------------------------------------------------------------------------
UI_EN = {
    # конфиги
    'Конфиг создан': 'Config created', 'Конфиг сохранён': 'Config saved',
    'Файл не найден': 'File not found', 'Несовместимый конфиг': 'Incompatible config',
    'Старый конфиг — пересохрани': 'Old config — save it again',
    'Конфиг загружен': 'Config loaded', 'Конфиг удалён': 'Config deleted',
    'Новый конфиг': 'New config', 'Создать конфиг': 'Create config',
    'Сохранённые': 'Saved', 'Нет конфигов': 'No configs', 'Загружен': 'Loaded',
    'Загрузить': 'Load', 'Сохранить': 'Save', 'Удалить конфиг?': 'Delete config?',
    'Конфиг': 'Config', '«%s»': '"%s"',
    'Будет удалён безвозвратно': 'Will be deleted permanently', 'Удалить': 'Delete',
    # аим
    'Аим': 'Aim', 'Только в прицеле': 'Only while scoped', 'Круг FOV': 'FOV circle',
    'Наводка': 'Aiming', 'Радиус': 'Radius', 'Скорость': 'Speed',
    'Куда целиться': 'Aim point', 'Голова': 'Head', 'Шея': 'Neck', 'Тело': 'Body',
    'Выбор цели': 'Target priority', 'Умный': 'Smart',
    'Ближе к прицелу': 'Closest to crosshair', 'Ближе ко мне': 'Closest to me',
    # тач-зона аима (точка, из которой аимбот водит палец)
    'Точка пальца': 'Finger point', 'Сбросить точку': 'Reset point',
    'Точка сброшена': 'Point reset',
    'Задай точку тапом по экрану': 'Set the point by tapping the screen',
    'Точка пальца сохранена': 'Finger point saved',
    'Тапни по точке, где аим водит палец': 'Tap the point the aim steers from',
    # esp
    'Противники': 'Enemies', 'Игроки': 'Players', 'Боксы': 'Boxes',
    '3D боксы': '3D boxes', 'Ники': 'Nicknames', 'Дистанция': 'Distance',
    'Линии': 'Lines', 'Скелеты': 'Skeletons', 'Свои': 'Teammates',
    'Линии и боксы': 'Lines and boxes', 'Толщина': 'Thickness',
    'Мир': 'World', 'Руда': 'Ore', 'Животные': 'Animals', 'Ящики': 'Crates',
    'Показывать до': 'Show up to', 'Ещё настройки': 'More settings',
    # автофарм
    'Автофарм': 'Auto-farm', 'Что добывать': 'What to farm', 'Дальность': 'Range',
    'Искать до': 'Search up to', '%.0f м': '%.0f m',
    'Зоны бота': 'Bot zones', 'Зона джойстика': 'Joystick zone',
    'Зона огня': 'Fire zone', 'Задать': 'Set', 'Зоны сброшены': 'Zones reset',
    'Сбросить зоны': 'Reset zones',
    'Тапни по центру джойстика движения': 'Tap the movement joystick centre',
    'Тапни по кнопке огня / атаки': 'Tap the fire / attack button',
    'Тап записывает зону. Меню откроется само.':
        'A tap records the zone. The menu reopens by itself.',
    'Зона джойстика сохранена': 'Joystick zone saved',
    'Зона огня сохранена': 'Fire zone saved',
    # версия игры (релиз/бета)
    'Версия игры': 'Game version', 'Релиз': 'Release', 'Бета': 'Beta',
    # привязка к игре (тост, когда привязаться не удалось)
    'Игра не найдена': 'Game not found',
    'Клиент не поддерживается': 'Client not supported',
    'Нет доступа к памяти игры': 'No access to the game memory',
    # разное и опции
    'Функции': 'Features', 'Иксрей': 'X-Ray', 'Всегда день': 'Always day',
    'Язык': 'Language', 'Интерфейс': 'Interface', 'Тёмная тема': 'Dark theme',
    'Панель вкладок': 'Tab bar', 'Слева': 'Left', 'Снизу': 'Bottom',
    'Панель слева': 'Panel on the left', 'Панель снизу': 'Panel at the bottom',
    'Система': 'System', 'Выйти?': 'Exit?', 'Выйти': 'Exit',
    'Приложение будет закрыто.': 'The app will close.', 'Готово': 'Done',
    'Отмена': 'Cancel', 'Включено': 'On', 'Выключено': 'Off',
    # вкладки
    'Меню': 'Menu', 'Разное': 'Misc', 'Конфиги': 'Configs', 'Опции': 'Options',
}

# ---------------------------------------------------------------------------
# Подписи визуалов, которых нет в таблицах игры: предметы, постройки, техника.
# ---------------------------------------------------------------------------
VISUAL_EN = {
    # ресурсы и материалы
    'Ящик': 'Crate', 'Бочка': 'Barrel', 'Железо': 'Iron', 'Металл': 'Metal',
    'Металл HQ': 'HQ Metal', 'Фрагменты': 'Metal Fragments', 'Листы': 'Sheet Metal',
    'Скрап': 'Scrap', 'Сера': 'Sulfur', 'Камень': 'Stone', 'Дерево': 'Wood',
    'Уголь': 'Charcoal', 'Порох': 'Gunpowder', 'Низкосорт': 'Low Grade',
    'Нефть': 'Crude Oil', 'Солярка': 'Diesel', 'Топливо': 'Fuel',
    'Электроника': 'Tech Trash', 'Батарея': 'Battery', 'Шестерни': 'Gears',
    'Пружина': 'Spring', 'Труба': 'Pipe', 'Лезвие': 'Blade', 'Верёвка': 'Rope',
    'Брезент': 'Tarp', 'Пропан': 'Propane', 'Швейный': 'Sewing Kit', 'Клей': 'Glue',
    'Скотч': 'Tape', 'Чертёж': 'Blueprint', 'Гаечный ключ': 'Wrench', 'Ключ': 'Key',
    'Ткань': 'Cloth', 'Кожа': 'Leather', 'Жир': 'Fat', 'Кости': 'Bone', 'Мясо': 'Meat',
    'Курятина': 'Chicken Meat', 'Мёд': 'Honey', 'Семена': 'Seeds', 'Яйцо': 'Egg',
    'Тунец': 'Tuna', 'Фасоль': 'Beans', 'Хлеб': 'Bread',
    # еда и лекарства
    'Консервы': 'Canned Food', 'Батончик': 'Chocolate Bar', 'Конфета': 'Candy',
    'Шоколад': 'Chocolate', 'Газировка': 'Soda', 'Вода': 'Water',
    'Бутылка воды': 'Water Bottle', 'Бутылка': 'Bottle', 'Яблоко': 'Apple',
    'Аптечка': 'Medkit', 'Шприц': 'Syringe', 'Бинт': 'Bandage', 'Антирад': 'Anti-Rad',
    'Таблетки': 'Pills',
    # собранное с кустов и грядок
    'Ягоды': 'Berries', 'Ягод': 'Berries', 'ягод': 'Berries', 'Грибы': 'Mushrooms',
    'Тыква': 'Pumpkin', 'Кукуруза': 'Corn', 'Картофель': 'Potato',
    'Куст ткани': 'Cloth Bush', 'Кактус': 'Cactus', 'Конопля': 'Hemp',
    # животные
    'Волк': 'Wolf', 'Крыса': 'Rat', 'Медведь': 'Bear', 'Кабан': 'Boar', 'Олень': 'Deer',
    'Кролик': 'Rabbit', 'Заяц': 'Hare', 'Курица': 'Chicken', 'Рыба': 'Fish',
    'Акула': 'Shark', 'Каннибал': 'Cannibal', 'Лошадь': 'Horse', 'Коза': 'Goat',
    'Овца': 'Sheep', 'Корова': 'Cow', 'Лиса': 'Fox', 'Змея': 'Snake',
    # расходники и прочее
    'Снежок': 'Snowball', 'Фейерверк': 'Firework', 'Канистра': 'Jerry Can',
    'Ведро': 'Bucket', 'Одежда': 'Clothing', 'Шлем': 'Helmet', 'Броня': 'Armor',
    'Патроны': 'Ammo', 'Стрелы': 'Arrows', 'Взрывчатка': 'Explosives',
    'Ракета': 'Rocket', 'Граната': 'Grenade', 'С4': 'C4', 'Дверь': 'Door',
    'План постройки': 'Building Plan', 'Оружие': 'Weapon', 'Предметы': 'Items',
    # ящики и контейнеры
    'Элитный ящик': 'Elite Crate', 'Редкий ящик': 'Rare Crate', 'Аирдроп': 'Airdrop',
    'Мед. ящик': 'Medical Crate', 'Ящик патронов': 'Ammo Crate',
    'Ящик инструментов': 'Toolbox', 'Ящик с едой': 'Food Crate',
    'Ящик с вертолёта': 'Heli Crate', 'Ящик с танка': 'Bradley Crate',
    'Ящик с вышки': 'Oil Rig Crate', 'Взломной ящик': 'Hackable Crate',
    'Сейф': 'Safe', 'Касса': 'Cash Register', 'Автомат': 'Vending Machine',
    'Контейнер': 'Container', 'Сундук': 'Chest', 'Кейс': 'Case', 'Тайник': 'Stash',
    'Мусорка': 'Trash Can', 'Военный ящик': 'Military Crate',
    # инструменты и короткие подписи по id предмета (switch в game.cpp)
    'Пила': 'Saw', 'Удочка': 'Fishing Rod', 'Бинокль': 'Binoculars',
    'Камера': 'Camera', 'Фонарик': 'Flashlight', 'Ракетница': 'Flare Gun',
    'Авиамаркер': 'Air Marker', 'Возд. маркер': 'Air Marker',
    'Винтовка': 'Rifle', 'Нож': 'Knife', 'Дигл': 'Deagle', 'Дубина': 'Club',
    'Дымовуха': 'Smoke Grenade', 'Отбойник': 'Jackhammer', 'ПП': 'SMG',
    'Самопал': 'Handmade Pistol', 'Шаромёт': 'Steel Ball Gun',
    'Дер. копьё': 'Wooden Spear', 'Жел. копьё': 'Iron Spear',
    'Кам. топорик': 'Stone Hatchet', 'Лед. копьё': 'Ice Spear',
    'Охот. винтовка': 'Hunting Rifle', 'Шип. дубина': 'Spiked Club',
}

# Подписи, которым игра даёт несколько английских имён: какое брать в оверлей.
WEAPON_EN_PICK = {
    'Молоток': 'Hammer',
    'Пулемёт': 'Machine Gun',
    'Снайперская винтовка': 'Sniper Rifle',
    'Топор': 'Hatchet',
}


def build_text():
    """Текст lang_tables.inc. Бросает SystemExit, если что-то не переведено."""
    game = strip_log_calls(strip_comments(open(GAME, encoding='utf-8').read()))
    main_src = strip_log_calls(strip_comments(open(MAIN, encoding='utf-8').read()))

    # --- оружие: имена берём из таблицы игры, колонка `en` -------------------
    visual = dict(VISUAL_EN)
    game_names = {}
    for _key, en, ru in re.findall(
            r'\{"([a-z0-9_\.]+)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\}', game):
        game_names.setdefault(ru, set()).add(en)
    for ru, names in game_names.items():
        pick = WEAPON_EN_PICK.get(ru)
        if pick is None:
            pick = sorted(names)[0]
        if pick not in names:
            raise SystemExit('WEAPON_EN_PICK: «%s» — игра такого имени не знает (%s)'
                             % (ru, sorted(names)))
        visual.setdefault(ru, pick)

    # --- полнота: ничего из кода не должно остаться без перевода -------------
    xs = {m.group(1) for m in re.finditer(r'(?<![\w:])(?:XS|XS_RU)\("((?:[^"\\]|\\.)*)"\)', main_src)}
    missing_ui = sorted(s for s in xs if has_cyr(s) and s not in UI_EN and s not in visual)
    if missing_ui:
        raise SystemExit('нет перевода для строк меню (добавь в UI_EN):\n    ' +
                         '\n    '.join(missing_ui))

    game_lits = {m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', game)
                 if has_cyr(m.group(1))}
    missing_vis = sorted(s for s in game_lits
                         if s not in visual and s not in UI_EN
                         and '%' not in s and not s.startswith(('#', ' ')))
    if missing_vis:
        raise SystemExit('нет перевода для подписей визуалов (добавь в VISUAL_EN):\n    ' +
                         '\n    '.join(missing_vis))

    buf = io.StringIO()
    w = buf.write
    w('// Таблицы перевода РУ/EN. Файл собран tools/lang/gen_tables.py — правь\n')
    w('// английские строки там (или в таблице оружия game.cpp), а не здесь.\n')
    w('//\n')
    w('// Сортировка по байтам UTF-8: по этому порядку идёт бинарный поиск (strcmp)\n')
    w('// в lang.cpp. Проверки полноты и сортировки — tools/lang/run.sh.\n\n')

    def esc(s):
        return s.replace('\\', '\\\\').replace('"', '\\"')

    def emit(name, table, comment):
        w(comment + '\n')
        w('static const LangPair %s[] = {\n' % name)
        for k in sorted(table, key=lambda s: s.encode('utf-8')):
            w('    {"%s", "%s"},\n' % (esc(k), esc(table[k])))
        w('};\n\n')

    emit('kUiEn', UI_EN,
         '// Интерфейс: русская строка (как она написана в коде) -> английская.\n'
         '// Форматные строки переводятся вместе с единицами: "%.0f м" -> "%.0f m".')
    emit('kVisualEn', visual,
         '// Подписи визуалов ESP: ресурсы, предметы, животные, ящики, оружие.\n'
         '// Английские имена оружия взяты из таблицы самой игры (kWeaponNames в\n'
         '// game.cpp), чтобы подпись совпадала с той, что игра пишет в своём UI.')
    return buf.getvalue(), visual


def main():
    text, visual = build_text()
    open(OUT, 'w', encoding='utf-8').write(text)
    print('lang_tables.inc: меню %d пар, визуалов %d пар' % (len(UI_EN), len(visual)))


if __name__ == '__main__':
    main()
