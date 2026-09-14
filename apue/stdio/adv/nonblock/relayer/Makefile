CFALGS+=-pthread
LDFLAGS+=-pthread
all:relayer

relayer:relayer.o main.o
	gcc $^ -o $@ $(CFALGS) $(LDFLAGS)

clean:
	rm -rf *.o relayer
