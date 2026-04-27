CC ?= $(CROSS_COMPILE)gcc

OBJS = main.o
CFLAGS += -Wall -Wextra
BINARY = qemu_tzc_core_isolation

.PHONY: all
all: $(BINARY)

$(BINARY): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDADD)

.PHONY: clean
clean:
	rm -f $(OBJS) $(BINARY)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
