#!/bin/sh
# Сторож аимбота: чем он имеет право мерить чувствительность камеры.
#
# Зачем отдельная проверка. Аим учит коэффициент «град поворота камеры на
# пиксель пальца» по ответу камеры. На устройстве настоящих углов нет вовсе
# (поза камеры и ось выстрела не читаются — cam_st = 0 во всех 304 строках
# аима в логе 15.09.2026), поэтому учиться ему не из чего, и работает запасное
# значение. Из логов автофарма чувствительность измерена: 0.10 град/px по yaw
# (35 свайпов, медиана; p10 0.078, p90 0.178). Прежнее запасное 0.35 было
# завышено в 3.5 раза — палец уходил в 3.5 раза меньше пикселей, чем нужно, и
# остаток ошибки закрывался не за 1-2 кадра, а за 12-13: в логе 16.09.2026 при
# exp = 0 ошибка падала 5.47 -> 0.35 град за 0.2 с. Снаружи это ровно «аим стал
# намного медленнее», поэтому проверка следит за тремя вещами:
#
#   1) UpdateAim берёт углы ТОЛЬКО у esp_aim_camera_angles (настоящая ось), а не
#      у esp_camera_angles с базисом из матрицы вида: базис отстаёт на кадр, по
#      нему аим выучивал коэффициент со сменой знака (0.072 -> 0.347 -> -0.072) и
#      слал палец на 162 px в край экрана — «аим дёргается». Фарму базис нужен и
#      остаётся;
#   2) запасной коэффициент аима равен измеренному и совпадает с запасным
#      коэффициентом камерного контроллера фарма (kCamGainProbe) — это одно и то
#      же число, измеренное по одному и тому же пальцу на одном экране;
#   3) в лог аима (exp) уходит произведение шага на РАБОЧИЙ коэффициент, а не на
#      выученный: иначе при запасном значении в логе стояли бы нули, и по строке
#      AIM нельзя было бы проверить, попадает ли коэффициент в чувствительность
#      игры;
#   4) доля ошибки, подаваемая аимом за такт, ограничена устойчивым пределом:
#      камера отвечает не в этом такте, а через 2 кадра (тот же замер, что и в
#      контроллере фарма), поэтому при доле -> 1 петля идёт по
#      err(t+1) = err(t) - k*err(t-1) с корнями |z| = sqrt(k) = 0.99 и
#      колебания +-3..10 градусов не затухают. В логе 16.09.2026 (сборка 6c44e8b)
#      доля была 0.98 — «аим быстрый, но очень сильно дергается».
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
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return '\n'.join(line.split('//')[0] for line in text.split('\n'))


def body_of(text, signature):
    start = text.index(signature)
    open_brace = text.index('{', start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace:i + 1]
    raise AssertionError('не найден конец ' + signature)


main = read('jni/src/main.cpp')
game = read('jni/src/game.cpp')

# 1. Тело UpdateAim: углы камеры берём только у аима.
aim_code = strip_comments(body_of(main, 'static void UpdateAim(float dt)'))

bad = [m.start() for m in re.finditer(r'\besp_camera_angles\s*\(', aim_code)]
if bad:
    fail.append('UpdateAim зовёт esp_camera_angles (базис из матрицы вида): '
                'углы аима — только esp_aim_camera_angles')
if len(re.findall(r'\besp_aim_camera_angles\s*\(', aim_code)) < 2:
    fail.append('в UpdateAim меньше двух вызовов esp_aim_camera_angles '
                '(обучение коэффициента и упреждение цели)')

# 2. Запасной коэффициент — измеренный, а не «высокий наугад».
field = {}
for name in ('probeGainYaw', 'probeGainPitch'):
    m = re.search(r'const float ' + name + r'\s*=\s*([0-9.]+)f', aim_code)
    if not m:
        fail.append('в UpdateAim нет запасного коэффициента ' + name)
        continue
    field[name] = float(m.group(1))
    value = field[name]
    if not (0.05 <= value <= 0.20):
        fail.append('%s = %.3f вне измеренной чувствительности 0.05..0.20 град/px '
                    '(0.35 держал шаг в 3.5 раза меньше нужного)' % (name, value))

farm_probe = re.search(r'kCamGainProbe\s*=\s*([0-9.]+)f', strip_comments(main))
if not farm_probe:
    fail.append('в main.cpp нет kCamGainProbe — запасного коэффициента фарма')
elif field.get('probeGainYaw') is not None:
    if abs(float(farm_probe.group(1)) - field['probeGainYaw']) > 1e-6:
        fail.append('запасной коэффициент аима (%.3f) не совпадает с запасным '
                    'коэффициентом фарма (%.3f): это одно и то же измеренное '
                    'число' % (field['probeGainYaw'], float(farm_probe.group(1))))

# 3. exp в логе — по рабочему коэффициенту.
if not re.search(r's_lastDx \* gy\b', aim_code) or not re.search(r'dx \* gy\b', aim_code):
    fail.append('строки AIM пишут exp по s_gainYaw (выученному) вместо gy/gp — '
                'при запасном коэффициенте в логе будут нули')

# 4. Доля ошибки за такт ограничена пределом устойчивости: при отклике камеры
#    через 2 кадра шаг почти в размер ошибки даёт незатухающие колебания.
m = re.search(r'const float kStable\s*=\s*([0-9.]+)f', aim_code)
if not m:
    fail.append('в UpdateAim нет предела доли ошибки за такт (kStable): при '
                'доле -> 1 петля раскачивается — «аим дёргается»')
else:
    stable = float(m.group(1))
    if not (0.3 <= stable <= 0.6):
        fail.append('kStable = %.2f вне устойчивого диапазона 0.3..0.6: выше 0.6 '
                    'остаются колебания, ниже 0.3 аим заметно медленнее' % stable)
    if not re.search(r'if \(k > kStable\) k = kStable;', aim_code):
        fail.append('предел kStable в UpdateAim не применён к k')

# 5. esp_aim_camera_angles не должен знать про базис из матрицы вида.
aim_angles = strip_comments(body_of(game, 'bool esp_aim_camera_angles('))
if 'g_frame_cam' in aim_angles:
    fail.append('esp_aim_camera_angles использует базис из матрицы вида '
                '(g_frame_cam_*): он отстаёт на кадр и годится только фарму')
for needed in ('g_aim_ref_valid', 'g_cam_pose_valid'):
    if needed not in aim_angles:
        fail.append('esp_aim_camera_angles не проверяет ' + needed)

# 6. Базис по-прежнему доступен фарму — иначе он снова повиснет в «нет позиции
#    камеры» на устройстве, где поза и ось не читаются.
farm_angles = strip_comments(body_of(game, 'bool esp_camera_angles('))
if 'g_frame_cam_basis_valid' not in farm_angles:
    fail.append('esp_camera_angles потерял базис из матрицы вида — фарм на '
                'устройстве без позы и оси останется без углов')

# 7. cam_st в логе обязан отражать источники аима: «cam_st 0 = настоящей оси нет»
#    должно читаться однозначно.
state = strip_comments(body_of(game, 'int esp_camera_state()'))
for needed in ('g_cam_pose_valid', 'g_aim_ref_valid'):
    if needed not in state:
        fail.append('esp_camera_state не отражает ' + needed)
if 'g_frame_cam' in state:
    fail.append('esp_camera_state подмешивает базис из матрицы вида: cam_st 0 '
                'перестанет означать «настоящей оси нет»')

if fail:
    print('проверка чувствительности и источника углов аима: ПРОВАЛ')
    for problem in fail:
        print('  ' + problem)
    sys.exit(1)

print('аим: запасной коэффициент %.3f град/px (измерен и совпадает с фармом), '
      'exp в логе по рабочему коэффициенту, углы — только настоящая ось '
      '(cam_st), базис из матрицы вида остаётся фарму, доля ошибки за такт '
      'ограничена %.2f' % (field.get('probeGainYaw', 0.0), stable))
PY
