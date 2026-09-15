#!/bin/sh
# Инвариант источника углов для аима: аим не имеет права учить чувствительность
# (и ждать ответа камеры) по базису из матрицы вида.
#
# Зачем отдельная проверка. Лог 15.09.2026 показал, как это ломается на
# устройстве: поза камеры и ось выстрела не читаются вовсе (cam_st = 0 во всех
# 304 строках аима), единственный доступный источник — базис из матрицы вида,
# который отстаёт на кадр. Пока аим брал углы у esp_camera_angles (общая функция
# с фармом), он по этому базису выучивал коэффициент со сменой знака
# (0.072 -> 0.347 -> -0.072) и слал палец на 162 px в край экрана — «аим
# дёргается». Фарму базис нужен и остаётся (медленный доворот по экранной
# метке), поэтому источники разведены: esp_camera_angles — фарм,
# esp_aim_camera_angles — аим, только настоящая ось.
#
# Проверка сторожит ровно это разделение — по исходникам, потому что собрать
# UpdateAim в хостовом стенде дороже, чем поймать сам факт регресса.
#
#   sh tools/aim/check.sh
set -e
cd "$(dirname "$0")/../.."

python3 -- "$@" <<'PY'
import re
import sys

fail = []


def read(path):
    return open(path, encoding='utf-8').read()


def strip_comments(text):
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S).replace('//', '')


main = read('jni/src/main.cpp')
game = read('jni/src/game.cpp')
header = read('jni/include/game.h')

# 1. Тело UpdateAim: углы камеры берём только у аима.
start = main.index('{', main.index('static void UpdateAim(float dt)'))
depth, end = 0, None
for i in range(start, len(main)):
    if main[i] == '{':
        depth += 1
    elif main[i] == '}':
        depth -= 1
        if depth == 0:
            end = i
            break
assert end, 'не найден конец UpdateAim'
aim_code = strip_comments(main[start:end + 1])

bad = [m.start() for m in re.finditer(r'\besp_camera_angles\s*\(', aim_code)]
if bad:
    fail.append('UpdateAim зовёт esp_camera_angles (базис из матрицы вида): '
                'углы аима — только esp_aim_camera_angles')
if len(re.findall(r'\besp_aim_camera_angles\s*\(', aim_code)) < 2:
    fail.append('в UpdateAim меньше двух вызовов esp_aim_camera_angles '
                '(обучение коэффициента и упреждение цели)')

# 2. esp_aim_camera_angles не должен знать про базис из матрицы вида.
a = game.index('bool esp_aim_camera_angles(')
b = game.index('\n}\n', a) + 3
body = strip_comments(game[a:b])
if 'g_frame_cam' in body:
    fail.append('esp_aim_camera_angles использует базис из матрицы вида '
                '(g_frame_cam_*): он отстаёт на кадр и годится только фарму')
for needed in ('g_aim_ref_valid', 'g_cam_pose_valid'):
    if needed not in body:
        fail.append('esp_aim_camera_angles не проверяет %s' % needed)

# 3. Базис по-прежнему доступен фарму — иначе он снова повиснет в «нет позиции
#    камеры» на устройстве, где поза и ось не читаются.
farm_angles_start = game.index('bool esp_camera_angles(')
farm_angles_end = game.index('\n}\n', farm_angles_start) + 3
farm_body = strip_comments(game[farm_angles_start:farm_angles_end])
if 'g_frame_cam_basis_valid' not in farm_body:
    fail.append('esp_camera_angles потерял базис из матрицы вида — фарм на '
                'устройстве без позы и оси останется без углов')

# 4. Лог аима: cam_st обязан быть маской настоящих источников (поза/ось), чтобы
#    «cam_st 0 = аим ведёт вслепую» читалось однозначно.
state_start = game.index('int esp_camera_state()')
state_end = game.index('\n}\n', state_start) + 3
state = strip_comments(game[state_start:state_end])
for needed in ('g_cam_pose_valid', 'g_aim_ref_valid'):
    if needed not in state:
        fail.append('esp_camera_state не отражает %s — по строке AIM не видно, '
                    'есть ли у аима настоящая ось' % needed)
if 'g_frame_cam' in state:
    fail.append('esp_camera_state подмешивает базис из матрицы вида: cam_st 0 '
                'перестанет означать «настоящей оси нет»')
if 'esp_aim_camera_angles' not in header:
    fail.append('game.h не объявляет esp_aim_camera_angles')

if fail:
    print('проверка источника углов аима: ПРОВАЛ')
    for problem in fail:
        print('  ' + problem)
    sys.exit(1)

print('источник углов аима: аим — только поза/ось выстрела (базис ему не '
      'отдаётся), фарм — с базисом из матрицы вида; cam_st отражает именно это')
PY
