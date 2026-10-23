.PHONY: all clean test run-sem run-cv

all:
	$(MAKE) -C sem
	$(MAKE) -C condvar

clean:
	$(MAKE) -C sem   clean
	$(MAKE) -C condvar clean

test: all
	bash run_tests.sh

run-sem: all
	sem/rw_sem -p fair -r 3 -w 2 -o 5

run-cv: all
	condvar/rw_cv -p fair -r 3 -w 2 -o 5
