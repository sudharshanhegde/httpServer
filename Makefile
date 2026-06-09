CC := gcc

CFLAGS := -Wall -Wextra -02 -g -D_GNU_SOURCE -pthread

INCLUDES := -Iinclude

SRC_DIR := src 
INC_DIR := include 
OBJ_DIR := obj 
BIN_DIR := bin 

OBJS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TARGET := $(BIN_DIR)/httpd

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR) $(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BIN_DIR): mkdir -p $(BIN_DIR)

$(OBJ_DIR): mkdir -p $(OBJ_DIR)

clean: rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: all clean