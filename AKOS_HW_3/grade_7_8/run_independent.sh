#!/usr/bin/env bash
# run_independent.sh — Запуск независимых программ-процессов (Master, Readers, Writers)
# Поддерживает передачу всех параметров в master

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

# Значения по умолчанию
READERS=3
WRITERS=2
ARGS=("$@")

# Извлечем число читателей и писателей из аргументов, если заданы
while [[ $# -gt 0 ]]; do
    case "$1" in
        -r|--readers) READERS="$2"; shift 2 ;;
        -w|--writers) WRITERS="$2"; shift 2 ;;
        *) shift ;;
    esac
done

echo "=== Запуск независимой системы (Оценка 7-8) ==="
echo "Читателей: $READERS, Писателей: $WRITERS"

# Очистим старые фоновые процессы при выходе или Ctrl+C
PIDS=()
cleanup() {
    echo ""
    echo "[SCRIPT] Завершение всех процессов..."
    for pid in "${PIDS[@]}"; do
        kill -SIGTERM "$pid" 2>/dev/null
    done
    wait 2>/dev/null
    echo "[SCRIPT] Все процессы остановлены."
}
trap cleanup SIGINT SIGTERM EXIT

# 1. Запуск Master-процесса в фоне
./master "${ARGS[@]}" &
MASTER_PID=$!
PIDS+=($MASTER_PID)

# Даем master немного времени на создание SHM и семафоров
sleep 0.2

# Проверяем, жив ли master
if ! kill -0 $MASTER_PID 2>/dev/null; then
    echo "[SCRIPT] Ошибка: Master завершился аварийно!"
    exit 1
fi

# 2. Запуск независимых процессов читателей
for ((i=0; i<READERS; i++)); do
    ./reader --id "$i" &
    PIDS+=($!)
done

# 3. Запуск независимых процессов писателей
for ((i=0; i<WRITERS; i++)); do
    ./writer --id "$i" &
    PIDS+=($!)
done

# 4. Ожидаем завершения Master
wait $MASTER_PID 2>/dev/null
EXIT_CODE=$?

# Убираем trap перед нормальным выходом
trap - SIGINT SIGTERM EXIT

# Дожидаемся завершения читателей и писателей
for pid in "${PIDS[@]}"; do
    if [ "$pid" != "$MASTER_PID" ]; then
        wait "$pid" 2>/dev/null || true
    fi
done

echo "=== Моделирование завершено (код возврата: $EXIT_CODE) ==="
exit $EXIT_CODE
