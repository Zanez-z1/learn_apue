
CFLAGS += -Wall -pthread 
LDFLAGS +=-pthread
all:mysem

mysem:main.o mysem.o
	gcc $(CFLAGS) $(LDFLAGS) $^ -o $@


clean:
	rm -rf *.o mysem
