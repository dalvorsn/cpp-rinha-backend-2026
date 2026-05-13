BUILD_DIR = build
BUILD_TYPE = Release
FORMAT = clang-format-19
TIDY = clang-tidy-19

SRCS := $(shell find src -name "*.cpp")
HDRS := $(shell find src -name "*.hpp")

.PHONY: all build format lint lint-fix install-deps clean

all: build

install-deps:
	@sudo apt-get update && sudo apt-get install -y \
		clang-19 \
		clang-format-19 \
		clang-tidy-19 \
		build-essential \
		cmake \
		git \
		libssl-dev \
		zlib1g-dev \
		libuv1-dev \
		gzip
	@if [ -f /.dockerenv ]; then \
		sudo rm -rf /var/lib/apt/lists/*; \
		echo "Docker environment detected: cleaned apt lists."; \
	fi

build:
	@cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build $(BUILD_DIR) -j$$(nproc)

format:
	@if [ -z "$(SRCS)" ] && [ -z "$(HDRS)" ]; then \
		echo "No files found in src/ to format."; \
	else \
		$(FORMAT) -i $(SRCS) $(HDRS); \
	fi

lint:
	@if [ ! -f $(BUILD_DIR)/compile_commands.json ]; then \
		cmake -B $(BUILD_DIR) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; \
	fi
	@if [ -n "$(SRCS)" ]; then \
		$(TIDY) $(SRCS) -p $(BUILD_DIR) -- -std=c++23; \
	fi

lint-fix:
	@if [ ! -f $(BUILD_DIR)/compile_commands.json ]; then \
		cmake -B $(BUILD_DIR) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; \
	fi
	@if [ -n "$(SRCS)" ]; then \
		$(TIDY) $(SRCS) -p $(BUILD_DIR) -fix -fix-errors -- -std=c++23; \
	fi

clean:
	@rm -rf $(BUILD_DIR)