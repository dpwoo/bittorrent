.PHONY: clean cleanall

CC = gcc
LDFLAGS =
CFLAGS =

RM = rm
RMFLAGS = -rf

MKDIR = mkdir

#BUILD_ROOT = $(shell pwd)
BUILD_ROOT = . 
BUILD_DIR := $(BUILD_ROOT)/build/

TARGET = $(BUILD_DIR)bittrent

SRCS = $(wildcard *.c)
OBJS = $(patsubst %.c, %.o, $(SRCS))
OBJS := $(addprefix $(BUILD_DIR), $(OBJS))

$(TARGET): $(BUILD_DIR) $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD_DIR):
	@$(MKDIR) $@

$(BUILD_DIR)%.o: %.c
	$(CC) -c $^ -o $@

clean:
	@$(RM) $(RMFLAGS) $(BUILD_DIR)*.o

cleanall: clean
	@$(RM) $(RMFLAGS) $(BUILD_DIR) $(TARGET)


