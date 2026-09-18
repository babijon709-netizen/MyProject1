#!/bin/sh
# Разбор лога оверлея с устройства: сводка, паузы (кандидаты во фриз),
# сторож, состояние оси сайлента, переключения режима аима.
#
#   sh tools/log/run.sh                      # самый свежий *.log в корне
#   sh tools/log/run.sh benzware_aim.log     # конкретный файл
set -e
cd "$(dirname "$0")/../.."
python3 tools/log/digest.py "$@"
