KDIR ?= /usr/src/linux
CC=$(CROSS_COMPILE)gcc
OBJS = v4l2_bridge
CFLAGS += -I$(KDIR)/usr/include -Wall -O2
LDFLAGS += -lpthread

all:  $(OBJS)

%.o : %.c
	$(CC) $(CFLAGS) -c $< -o $@ $(LDFLAGS)

% : %.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f *.o
	rm -f $(OBJS)
