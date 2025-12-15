# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lbluetooth

# Target executables
SERVER = btfs_server
CLIENT = btfs_client

# Source files
SERVER_SRC = btfs_server.c
CLIENT_SRC = btfs_client.c

# Object files
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Default target: build both programs
all: $(SERVER) $(CLIENT)

# Build server
$(SERVER): $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Build client
$(CLIENT): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile .c files to .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(SERVER) $(CLIENT) *.o

# Install targets (optional)
install: all
	install -m 755 $(SERVER) /usr/local/bin/
	install -m 755 $(CLIENT) /usr/local/bin/

# Uninstall (optional)
uninstall:
	rm -f /usr/local/bin/$(SERVER)
	rm -f /usr/local/bin/$(CLIENT)

# Build only server
server: $(SERVER)

# Build only client
client: $(CLIENT)

# Rebuild everything
rebuild: clean all

.PHONY: all clean install uninstall server client rebuild
