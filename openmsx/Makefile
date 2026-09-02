.PHONY: all selftest neo-test input-test

all: openmsx
	chmod +x openmsx
	./openmsx version >/dev/null

selftest: openmsx
	./openmsx selftest
	./openmsx neo-test
	python3 input_test.py ./openmsx

neo-test: openmsx
	python3 neo_mapper_test.py openmsx

input-test: openmsx
	python3 input_test.py ./openmsx
