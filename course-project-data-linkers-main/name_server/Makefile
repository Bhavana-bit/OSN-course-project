CC = gcc
CFLAGS = -Wall -Wextra -g

SRC = nm.c index.c search.c logging.c errors.c
OBJ = $(SRC:.c=.o)

TARGET = nm

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

# Recompile when any header changes
%.o: %.c logging.h errors.h index.h search.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJ) $(TARGET)

run:
	./$(TARGET)
