
CFLAGS += -Wall -pthread
LDFLAGS += -pthread
all:anytimer

anytimer:main.o anytimer.o
	gcc $(CFLAGS) $(LDFLAGS) $^ -o $@


clean:
	rm -rf *.o anytimer
