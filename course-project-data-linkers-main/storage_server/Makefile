CC = gcc
CFLAGS = -Wall -Wextra -g

SRC = main.c utils.c undo.c sentences.c write_handler.c file_ops.c logging.c errors.c
OBJ = $(SRC:.c=.o)

TARGET = ss

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

# Object files depend on all headers
%.o: %.c server.h logging.h errors.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	./$(TARGET)
