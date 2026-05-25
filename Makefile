FORMAT = clang-format-19
TIDY   = clang-tidy-19

SRCS := $(shell find src -name "*.cpp")
HDRS := $(shell find src -name "*.hpp")

.PHONY: all build run run-apm down convert cmake lint lint-fix format install-deps clean prune

all: build

build:
	docker compose build

run:
	docker compose up --build -d

run-apm:
	docker compose --profile monitoring up --build -d

down:
	docker compose down

convert:
	@cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build build -j$$(nproc)
	@if [ -f resources/references.json.gz ] && [ ! -f resources/references.json ]; then \
		gunzip -k resources/references.json.gz; \
	fi
	@./build/converter resources/references.json resources/references.bin

cmake:
	@cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build build -j$$(nproc)

format:
	@$(FORMAT) -i $(SRCS) $(HDRS)

lint:
	@if [ ! -f build/compile_commands.json ]; then \
		cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; \
	fi
	@$(TIDY) $(SRCS) -p build -- -std=c++23

lint-fix:
	@if [ ! -f build/compile_commands.json ]; then \
		cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; \
	fi
	@$(TIDY) $(SRCS) -p build -fix -fix-errors -- -std=c++23

install-deps:
	@sudo apt-get update && sudo apt-get install -y \
		build-essential \
		cmake \
		git \
		gzip \
		clang-19 \
		clang-format-19 \
		clang-tidy-19
	@if [ -f /.dockerenv ]; then \
		sudo rm -rf /var/lib/apt/lists/*; \
	fi

clean:
	@rm -rf build

prune: down
	docker compose --profile monitoring down --rmi all --volumes --remove-orphans
	docker image prune -f
	@rm -rf build
