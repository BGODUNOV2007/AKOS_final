#!/usr/bin/env bash
# run_tests.sh — Комплексный автотест всех компонентов (оценки 4-5, 6, 7-8, 9-10)
# Проверяет сборку, выполнение всех политик, соблюдение инвариантов и корректность обработки SIGINT

set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

LOG_FILE="test_results.log"
PASSED=0
FAILED=0

# Предварительная очистка и подготовка до запуска логгера
make clean >/dev/null 2>&1 || true
rm -f /dev/shm/*akos_hw3* /dev/mqueue/*akos_hw3* "$LOG_FILE" 2>/dev/null || true

run_test() {
    local test_name="$1"
    local cmd="$2"
    echo -n "[TEST] $test_name ... "
    
    local out
    out=$(eval "$cmd" 2>&1)
    local status=$?
    
    if [ $status -ne 0 ]; then
        echo "FAIL (код возврата: $status)"
        echo "$out"
        FAILED=$((FAILED + 1))
        return 1
    fi

    # Проверка на нарушение инвариантов в выводе
    if echo "$out" | grep -q "INVARIANT VIOLATION"; then
        echo "FAIL (обнаружено нарушение инварианта!)"
        echo "$out" | grep "INVARIANT VIOLATION"
        FAILED=$((FAILED + 1))
        return 1
    fi

    if echo "$out" | grep -q "ЕСТЬ НАРУШЕНИЯ"; then
        echo "FAIL (статистика зафиксировала нарушения инвариантов!)"
        FAILED=$((FAILED + 1))
        return 1
    fi

    echo "PASS"
    PASSED=$((PASSED + 1))
    return 0
}

run_sigint_test() {
    local test_name="$1"
    local start_cmd="$2"
    echo -n "[TEST] $test_name (SIGINT graceful shutdown) ... "

    # Гарантируем чистоту перед запуском
    rm -f /dev/shm/*akos_hw3* /dev/mqueue/*akos_hw3* 2>/dev/null || true

    eval "exec $start_cmd" >/dev/null 2>&1 &
    local pid=$!
    sleep 0.5
    kill -INT "$pid" 2>/dev/null || true
    
    local count=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.1
        count=$((count + 1))
        if [ $count -gt 30 ]; then
            kill -KILL "$pid" 2>/dev/null || true
            break
        fi
    done
    wait "$pid" 2>/dev/null || true
    sleep 0.2

    # Проверяем, что в /dev/shm и /dev/mqueue не осталось неудаленных ресурсов
    local shm_left
    shm_left=$(find /dev/shm -maxdepth 1 -name "*akos_hw3*" 2>/dev/null)
    local mq_left
    mq_left=$(find /dev/mqueue -maxdepth 1 -name "*akos_hw3*" 2>/dev/null)

    if [ -n "$shm_left" ] || [ -n "$mq_left" ]; then
        echo "FAIL (обнаружены неудаленные ресурсы!)"
        [ -n "$shm_left" ] && echo "  В /dev/shm: $shm_left"
        [ -n "$mq_left" ] && echo "  В /dev/mqueue: $mq_left"
        rm -f /dev/shm/*akos_hw3* /dev/mqueue/*akos_hw3* 2>/dev/null || true
        FAILED=$((FAILED + 1))
        return 1
    fi

    echo "PASS"
    PASSED=$((PASSED + 1))
    return 0
}

run_sigint_independent_test() {
    local test_name="$1"
    local coord_cmd="$2"
    local reader_cmd="$3"
    local writer_cmd="$4"
    echo -n "[TEST] $test_name (SIGINT graceful shutdown) ... "

    rm -f /dev/shm/*akos_hw3* /dev/mqueue/*akos_hw3* 2>/dev/null || true

    eval "exec $coord_cmd" >/dev/null 2>&1 &
    local cpid=$!
    sleep 0.2

    eval "exec $reader_cmd" >/dev/null 2>&1 &
    local rpid=$!

    eval "exec $writer_cmd" >/dev/null 2>&1 &
    local wpid=$!

    sleep 0.5
    kill -INT $cpid $rpid $wpid 2>/dev/null || true

    local count=0
    while kill -0 $cpid 2>/dev/null || kill -0 $rpid 2>/dev/null || kill -0 $wpid 2>/dev/null; do
        sleep 0.1
        count=$((count + 1))
        if [ $count -gt 30 ]; then
            kill -KILL $cpid $rpid $wpid 2>/dev/null || true
            break
        fi
    done
    wait $cpid 2>/dev/null || true
    wait $rpid 2>/dev/null || true
    wait $wpid 2>/dev/null || true
    sleep 0.2

    local shm_left
    shm_left=$(find /dev/shm -maxdepth 1 -name "*akos_hw3*" 2>/dev/null)
    local mq_left
    mq_left=$(find /dev/mqueue -maxdepth 1 -name "*akos_hw3*" 2>/dev/null)

    if [ -n "$shm_left" ] || [ -n "$mq_left" ]; then
        echo "FAIL (обнаружены неудаленные ресурсы!)"
        [ -n "$shm_left" ] && echo "  В /dev/shm: $shm_left"
        [ -n "$mq_left" ] && echo "  В /dev/mqueue: $mq_left"
        rm -f /dev/shm/*akos_hw3* /dev/mqueue/*akos_hw3* 2>/dev/null || true
        FAILED=$((FAILED + 1))
        return 1
    fi

    echo "PASS"
    PASSED=$((PASSED + 1))
    return 0
}

run_expected_fail_test() {
    local test_name="$1"
    local cmd="$2"
    echo -n "[TEST] $test_name ... "

    eval "$cmd" >/dev/null 2>&1
    local status=$?
    if [ $status -eq 0 ]; then
        echo "FAIL (ожидалась ошибка валидации, но код возврата 0)"
        FAILED=$((FAILED + 1))
        return 1
    fi

    echo "PASS"
    PASSED=$((PASSED + 1))
    return 0
}

run_all_tests() {
echo "================================================================="
echo "   АВТОМАТИЧЕСКОЕ ТЕСТИРОВАНИЕ ИНДИВИДУАЛЬНОГО ЗАДАНИЯ №3       "
echo "   Вариант 2: Читатели и писатели (исключительный доступ)        "
echo "================================================================="
echo "Дата запуска: $(date)"
echo ""

# 1. Сборка всех модулей
echo ">>> Этап 1: Проверка компиляции..."
if ! make all >/dev/null 2>&1; then
    echo "Ошибка сборки make all!"
    return 1
fi
echo "Все модули успешно скомпилированы."
echo ""

# 2. Тесты для Grade 4-5
echo ">>> Этап 2: Тестирование версии на 4-5 баллов (именованные семафоры + shm)..."
run_test "Grade 4-5: Политика Fair (3 читателя, 2 писателя)" \
    "./grade_4_5/readers_writers_4_5 -r 3 -w 2 -s 8 -o 3 -p fair --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 4-5: Политика Readers Preference (4 читателя, 1 писатель)" \
    "./grade_4_5/readers_writers_4_5 -r 4 -w 1 -s 6 -o 3 -p readers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 4-5: Политика Writers Preference (2 читателя, 3 писателя)" \
    "./grade_4_5/readers_writers_4_5 -r 2 -w 3 -s 6 -o 3 -p writers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 4-5: Запуск по файлу конфигурации configs/sample_fair.conf" \
    "./grade_4_5/readers_writers_4_5 -c configs/sample_fair.conf"

run_test "Grade 4-5: Пользовательский массив и диапазон индексов (--init-array, --idx-min/max)" \
    "./grade_4_5/readers_writers_4_5 -r 2 -w 2 --init-array '25,5,15,10,30' --idx-min 1 --idx-max 3 -o 3 -p fair --delay-min 5 --delay-max 15 --work-min 5 --work-max 10"

run_sigint_test "Grade 4-5: Прерывание по SIGINT" \
    "./grade_4_5/readers_writers_4_5 -r 2 -w 1 -o 0 --delay-min 10 --delay-max 20 --work-min 10 --work-max 20"
echo ""

# 3. Тесты для Grade 6
echo ">>> Этап 3: Тестирование версии на 6 баллов (неименованные семафоры в shm)..."
run_test "Grade 6: Политика Fair (3 читателя, 2 писателя)" \
    "./grade_6/readers_writers_6 -r 3 -w 2 -s 8 -o 3 -p fair --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 6: Политика Readers Preference" \
    "./grade_6/readers_writers_6 -r 3 -w 2 -s 8 -o 3 -p readers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 6: Политика Writers Preference" \
    "./grade_6/readers_writers_6 -r 2 -w 3 -s 8 -o 3 -p writers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 6: Запуск по конфигурационному файлу" \
    "./grade_6/readers_writers_6 -c configs/sample_writers.conf"

run_sigint_test "Grade 6: Прерывание по SIGINT" \
    "./grade_6/readers_writers_6 -r 2 -w 1 -o 0 --delay-min 10 --delay-max 20 --work-min 10 --work-max 20"
echo ""

# 4. Тесты для Grade 7-8
echo ">>> Этап 4: Тестирование версии на 7-8 баллов (независимые процессы + shm)..."
run_test "Grade 7-8: Независимые процессы (Fair)" \
    "./grade_7_8/run_independent.sh -r 3 -w 2 -s 7 -o 3 -p fair --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 7-8: Независимые процессы (Readers Preference)" \
    "./grade_7_8/run_independent.sh -r 4 -w 1 -s 7 -o 3 -p readers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 7-8: Независимые процессы (Writers Preference)" \
    "./grade_7_8/run_independent.sh -r 2 -w 3 -s 7 -o 3 -p writers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_sigint_independent_test "Grade 7-8: Независимые процессы" \
    "./grade_7_8/master -r 2 -w 1 -o 0 --delay-min 10 --delay-max 20 --work-min 10 --work-max 20" \
    "./grade_7_8/reader --id 0" \
    "./grade_7_8/writer --id 0"
echo ""

# 5. Тесты для Grade 9-10
echo ">>> Этап 5: Тестирование версии на 9-10 баллов (очереди сообщений POSIX + семафоры)..."
run_test "Grade 9-10: POSIX Message Queues (Fair)" \
    "./grade_9_10/run_independent_mq.sh -r 3 -w 2 -s 7 -o 3 -p fair --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 9-10: POSIX Message Queues (Readers Preference)" \
    "./grade_9_10/run_independent_mq.sh -r 4 -w 1 -s 7 -o 3 -p readers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 9-10: POSIX Message Queues (Writers Preference)" \
    "./grade_9_10/run_independent_mq.sh -r 2 -w 3 -s 7 -o 3 -p writers --delay-min 10 --delay-max 25 --work-min 10 --work-max 20"

run_test "Grade 9-10: Пользовательский массив и диапазон индексов (--init-array, --idx-min/max)" \
    "./grade_9_10/run_independent_mq.sh -r 2 -w 2 --init-array '20,5,15,10,30' --idx-min 1 --idx-max 3 -o 3 -p fair --delay-min 5 --delay-max 15 --work-min 5 --work-max 10"

run_sigint_independent_test "Grade 9-10: POSIX Message Queues" \
    "./grade_9_10/coordinator -r 2 -w 1 -o 0 --delay-min 10 --delay-max 20 --work-min 10 --work-max 20" \
    "./grade_9_10/reader --id 0" \
    "./grade_9_10/writer --id 0"
echo ""

echo ">>> Этап 6: Граничные случаи и валидация входных данных..."
run_expected_fail_test "CLI: Отклонение несуществующего файла конфигурации" \
    "./grade_4_5/readers_writers_4_5 -c configs/no_such_file.conf"
run_expected_fail_test "CLI: Отклонение нулевого числа читателей (-r 0)" \
    "./grade_4_5/readers_writers_4_5 -r 0"
run_expected_fail_test "CLI: Отклонение неположительного элемента в --init-array" \
    "./grade_4_5/readers_writers_4_5 --init-array '5,0,-3'"
run_expected_fail_test "CLI: Отклонение превышения лимита размера для POSIX MQ (-s 500)" \
    "./grade_9_10/coordinator -s 500"
echo ""

echo "================================================================="
echo "   РЕЗУЛЬТАТЫ ТЕСТИРОВАНИЯ: УСПЕШНО: $PASSED / НЕУДАЧ: $FAILED   "
echo "================================================================="

if [ $FAILED -eq 0 ]; then
    echo "ВСЕ ТЕСТЫ ПРОЙДЕНЫ НА 100%! Проект полностью готов к сдаче."
    return 0
else
    echo "ОБНАРУЖЕНЫ ОШИБКИ В ТЕСТАХ!"
    return 1
fi
}

run_all_tests 2>&1 | tee "$LOG_FILE"
exit "${PIPESTATUS[0]}"
