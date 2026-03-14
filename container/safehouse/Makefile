CC      = gcc
CFLAGS  = -Wall -Wextra -O2
BINDIR  = bin

SRC = safehouse_wrap.c
BIN = $(BINDIR)/safehouse_wrap

.PHONY: all clean

all: $(BIN)

$(BINDIR):
	mkdir -p $(BINDIR)

$(BIN): $(SRC) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BINDIR)
