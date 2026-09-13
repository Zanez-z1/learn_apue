
CFLAGS += -Wall -pthread 
LDFLAGS +=-pthread
all:mytbf

mytbf:main.o mytbf.o
	gcc $(CFLAGS) $(LDFLAGS) $^ -o $@


clean:
	rm -rf *.o mytbf
