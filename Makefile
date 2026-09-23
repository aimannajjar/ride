.PHONY: all gdb clean analyze perf_record perf_stat release test

NAME 				:= ride
BUILD_DIR		:= build
SRCS				:= $(wildcard src/*.c) src/log.h
BPF_SRCS 		:= $(wildcard src/*.bpf.c)
BPF_OBJS		:= $(BPF_SRCS:src/%.c=$(BUILD_DIR)/%.o)
BPF_INCLUDE := -I${LINUX} -I${LIBBPF} -I./src
BPF_FLAGS		:= $(BPF_INCLUDE) -target bpf -g -O3 -std=gnu11 -c 
BPF_CC 			:= clang

## default to debug build for dev
all: debug

## userspace builds
debug: $(BUILD_DIR)/$(NAME).skel.h 
	cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build build

release: $(BUILD_DIR)/$(NAME).skel.h 
	cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build

## tests & dev helpers
gdb: debug
	sudo gdb --args ./build/ride ./tests/fixtures/test1.txt -t 1 -v

test: release
	./tests/6_smoke_test_correctness.sh

## perf and static analyze targets
analyze: $(BUILD_DIR)/$(NAME).skel.h $(SRCS)
	cmake --preset analyze
	cmake --build --preset analyze

perf_record: $(BUILD_DIR)/$(NAME)
	bash ./tests/0_perf_record.sh

perf_stat: $(BUILD_DIR)/$(NAME)
	bash ./tests/0_perf_stat.sh

## bpf program
$(BUILD_DIR)/$(NAME).skel.h: $(BPF_OBJS) | $(BUILD_DIR)
	bpftool gen skeleton $< > $@

$(BPF_OBJS): $(BPF_SRCS) | $(BUILD_DIR)
	$(BPF_CC) $(BPF_FLAGS) $< -o $@


## build dir
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf *.o $(NAME).skel.h build
