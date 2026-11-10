#!/usr/bin/env bash
# run_independent_mq.sh — Запуск независимой системы на очередях сообщений POSIX (Оценка 9-10)

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

READERS=3
WRITERS=2
ARGS=("$@")

while [[ $# -gt 0 ]]; do
    case "$1" in
        -r|--readers) READERS="$2"; shift 2 ;;
        -w|--writers) WRITERS="$2"; shift 2 ;;
        *) shift ;;
    esac
done

echo "=== Запуск независимой системы на очередях сообщений POSIX (Оценка 9-10) ==="
echo "Читателей: $READERS, Писателей: $WRITERS"

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

# 1. Запуск координатора в фоне
./coordinator "${ARGS[@]}" &
COORD_PID=$!
PIDS+=($COORD_PID)

sleep 0.2

if ! kill -0 $COORD_PID 2>/dev/null; then
    echo "[SCRIPT] Ошибка: Координатор завершился аварийно!"
    exit 1
fi

# 2. Запуск читателей
for ((i=0; i<READERS; i++)); do
    ./reader --id "$i" &
    PIDS+=($!)
done

# 3. Запуск писателей
for ((i=0; i<WRITERS; i++)); do
    ./writer --id "$i" &
    PIDS+=($!)
done

# 4. Ожидание завершения координатора
wait $COORD_PID 2>/dev/null
EXIT_CODE=$?

# Убираем trap перед нормальным выходом
trap - SIGINT SIGTERM EXIT

# Дожидаемся завершения читателей и писателей
for pid in "${PIDS[@]}"; do
    if [ "$pid" != "$COORD_PID" ]; then
        wait "$pid" 2>/dev/null || true
    fi
done

echo "=== Моделирование завершено (код возврата: $EXIT_CODE) ==="
exit $EXIT_CODE
