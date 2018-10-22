CC=gcc
CFLAGS=-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wconversion
DEPS = 
OBJ = ifacemonitor.o
OUTPUT = ifacemonitor
LIBS = -lmosquitto -lpthread

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

default: $(OBJ)
	gcc $^ $(CFLAGS) -o $(OUTPUT) $(LIBS)

clean:
	rm $(OUTPUT)
	rm *.o
