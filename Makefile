CC = gcc
CFLAGS = -O3 -march=native -mtune=native -flto -std=c99 \
         -Wall -Wextra -Werror \
         -ffast-math -funroll-loops -fomit-frame-pointer \
         -finline-functions -finline-small-functions \
         -findirect-inlining -fprefetch-loop-arrays \
         -ftree-vectorize -fvect-cost-model=dynamic \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
         -D_POSIX_C_SOURCE=199309L -D_GNU_SOURCE

LDFLAGS = -flto -Wl,-O3 -Wl,--as-needed -Wl,--gc-sections \
          -Wl,-z,relro -Wl,-z,now

SRC_DIR = src
TARGET = fuse
SOURCE = $(SRC_DIR)/fuse.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SOURCE) $(shell pkg-config --cflags --libs fuse3)
	strip --strip-all $(TARGET)
	@echo "Build completed successfully!"

clean:
	rm -f $(TARGET)

.PHONY: all clean
