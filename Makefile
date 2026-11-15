# CampusCompute - build file
# Usage: make | make test | make clean | make zip SRN=PES1UGXXXXXXX
CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=gnu11 -O2 -g
LDLIBS  = -pthread -lrt
TARGET  = campuscompute
SRC     = campuscompute.c
PKGFILES = campuscompute.c Makefile README.md jobs.txt jobs_idle.txt bad_jobs.txt \
           banker_unsafe.txt banker_bad.txt test.sh

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: $(TARGET)
	./test.sh

# Removes only files created by this project (binary and its log file).
clean:
	rm -f $(TARGET) campuscompute.log

zip: clean
	@test -n "$(SRN)" || (echo "usage: make zip SRN=<your full SRN>"; exit 1)
	zip $(SRN).zip $(PKGFILES)
	unzip -t $(SRN).zip

.PHONY: all test clean zip
