PROGRAM = rmate

SRCS = $(PROGRAM).c
OBJS = $(SRCS:.c=.o)
RM ?= rm -f

# Build id: YYYYMMDD-<short_hash>[-dirty].
# Empty (and omitted from CFLAGS) when git is unavailable
BUILD_ID := $(shell v=$$(git log -1 --format='%cd-%h' --date=format:'%Y%m%d' 2>/dev/null) && { git diff --quiet 2>/dev/null || v="$$v-dirty"; } && echo "$$v")

CFLAGS += -std=gnu11 -O2 -Wall -Wextra -Wno-missing-field-initializers
ifneq ($(BUILD_ID),)
CFLAGS += -DBUILD_ID=\"+$(BUILD_ID)\"
endif

PREFIX ?= /usr/bin
DESTDIR ?= /
PYTHON ?= python3

.SUFFIXES: .o
.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

all: $(PROGRAM)

$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $? -o $@

check: $(PROGRAM)
	RMATE=./$(PROGRAM) $(PYTHON) tests/test_rmate.py

clean:
	$(RM) $(PROGRAM) $(OBJS)

install: $(PROGRAM)
	install -d $(DESTDIR)/$(PREFIX)
	install $(PROGRAM) $(DESTDIR)/$(PREFIX)/$(PROGRAM)

.PHONY: clean all check
