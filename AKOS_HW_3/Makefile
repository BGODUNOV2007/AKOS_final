# Корневой Makefile для проекта AKOS_HW_3 (Индивидуальное задание №3)
# Вариант 2: Читатели и писатели (исключительный доступ писателя)
# Студент: Годунов Борис Эдуардович, группа 252

SUBDIRS = grade_4_5 grade_6 grade_7_8 grade_9_10

all:
	@for dir in $(SUBDIRS); do \
		echo "=== Сборка $$dir ==="; \
		$(MAKE) -C $$dir || exit 1; \
	done
	@echo "=== Все компоненты успешно собраны ==="

clean:
	@for dir in $(SUBDIRS); do \
		echo "=== Очистка $$dir ==="; \
		$(MAKE) -C $$dir clean; \
	done
	rm -f *.log test_results.log
	rm -f /dev/shm/*akos_hw3* /dev/mqueue/*akos_hw3* 2>/dev/null || true
	@echo "=== Очистка завершена ==="

test: all
	@echo "=== Запуск автоматических тестов ==="
	./run_tests.sh

.PHONY: all clean test $(SUBDIRS)
