.PHONY: all gdb clean analyze perf_record perf_stat release testd debug trace debug_quiet

NAME        := ride
BUILD_DIR   := build/debug
SRCS        := $(wildcard src/*.c) src/log.h
BPF_SRCS    := $(wildcard src/*.bpf.c)
BPF_OBJS    := $(BPF_SRCS:src/%.c=$(BUILD_DIR)/%.o)
BPF_INCLUDE := -I${LINUX} -I${LIBBPF} -I./src
BPF_FLAGS   := $(BPF_INCLUDE) -target bpf -g -O3 -std=gnu11 -c 
BPF_CC      := clang

## default to debug build for dev
all: debug

## userspace builds
debug: $(BUILD_DIR)/$(NAME).skel.h 
	cmake --preset=debug
	cmake --build --preset=debug
	ln -sf $(PWD)/build/debug/ride $(PWD)/build/ride
	cp build/debug/compile_commands.json ./

release: $(BUILD_DIR)/$(NAME).skel.h 
	cmake --preset=release
	cmake --build --preset=release
	ln -sf $(PWD)/build/release/ride $(PWD)/build/ride

debug_quiet: $(BUILD_DIR)/$(NAME).skel.h 
	cmake --preset=debug_quiet
	cmake --build --preset=debug_quiet
	ln -sf $(PWD)/build/debug_quiet/ride $(PWD)/build/ride

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
