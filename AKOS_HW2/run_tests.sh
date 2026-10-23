#!/bin/bash
# Скрипт автоматического тестирования обеих версий программы
# Проверяет корректность работы всех трёх политик

set -e
PASS=0
FAIL=0

run_test() {
    local name="$1"
    local binary="$2"
    local args="$3"
    echo -n "  [TEST] $name ... "
    output=$($binary $args 2>&1)
    if echo "$output" | grep -q "Все потоки завершили работу"; then
        echo "PASS"
        PASS=$((PASS+1))
    else
        echo "FAIL"
        echo "    Вывод: $output" | head -5
        FAIL=$((FAIL+1))
    fi
}

echo "======================================"
echo "  Сборка проектов"
echo "======================================"
make -C sem   clean all 2>&1 | tail -2
make -C condvar clean all 2>&1 | tail -2
echo ""

echo "======================================"
echo "  Тестирование: sem/ (семафоры)"
echo "======================================"
run_test "fair  policy (3R 2W 3ops)" sem/rw_sem "-p fair    -r 3 -w 2 -o 3 --sleep-min 10 --sleep-max 30"
run_test "readers policy (5R 1W 2ops)" sem/rw_sem "-p readers -r 5 -w 1 -o 2 --sleep-min 10 --sleep-max 30"
run_test "writers policy (1R 4W 2ops)" sem/rw_sem "-p writers -r 1 -w 4 -o 2 --sleep-min 10 --sleep-max 30"
run_test "single reader" sem/rw_sem "-p fair -r 1 -w 0 -o 3 --sleep-min 10 --sleep-max 20"
run_test "single writer" sem/rw_sem "-p fair -r 0 -w 1 -o 3 --sleep-min 10 --sleep-max 20"
run_test "log file"    sem/rw_sem "-p fair -r 2 -w 1 -o 2 --sleep-min 10 --sleep-max 20 --log /tmp/test_sem.log"
echo ""

echo "======================================"
echo "  Тестирование: condvar/ (условные переменные)"
echo "======================================"
run_test "fair  policy (3R 2W 3ops)" condvar/rw_cv "-p fair    -r 3 -w 2 -o 3 --sleep-min 10 --sleep-max 30"
run_test "readers policy (5R 1W 2ops)" condvar/rw_cv "-p readers -r 5 -w 1 -o 2 --sleep-min 10 --sleep-max 30"
run_test "writers policy (1R 4W 2ops)" condvar/rw_cv "-p writers -r 1 -w 4 -o 2 --sleep-min 10 --sleep-max 30"
run_test "single reader" condvar/rw_cv "-p fair -r 1 -w 0 -o 3 --sleep-min 10 --sleep-max 20"
run_test "single writer" condvar/rw_cv "-p fair -r 0 -w 1 -o 3 --sleep-min 10 --sleep-max 20"
run_test "log file"    condvar/rw_cv "-p fair -r 2 -w 1 -o 2 --sleep-min 10 --sleep-max 20 --log /tmp/test_cv.log"
echo ""

echo "======================================"
echo "  Итог: PASS=$PASS FAIL=$FAIL"
echo "======================================"
[ $FAIL -eq 0 ] && exit 0 || exit 1
