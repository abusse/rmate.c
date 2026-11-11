PROGRAM = rmate

SRCS = $(PROGRAM).c
OBJS = $(SRCS:.c=.o)
RM ?= rm -f

# Build id: YYYYMMDD-<short_hash>[-dirty].
# Empty (and omitted from CFLAGS) when git is unavailable
BUILD_ID := $(shell v=$$(git log -1 --format='%cd-%h' --date=format:'%Y%m%d' 2>/dev/null) && { git diff --quiet 2>/dev/null || v="$$v-dirty"; } && echo "$$v")

# -O2 is only the default: CFLAGS from the environment or the command line win.
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers
ifneq ($(BUILD_ID),)
CPPFLAGS += -DBUILD_ID=\"+$(BUILD_ID)\"
endif

PREFIX ?= /usr/bin
DESTDIR ?= /
PYTHON ?= python3

.SUFFIXES: .o
.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

all: $(PROGRAM)

$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

check: $(PROGRAM)
	RMATE=./$(PROGRAM) $(PYTHON) tests/test_rmate.py

clean:
	$(RM) $(PROGRAM) $(OBJS)

install: $(PROGRAM)
	install -d $(DESTDIR)/$(PREFIX)
	install $(PROGRAM) $(DESTDIR)/$(PREFIX)/$(PROGRAM)

.PHONY: clean all check
