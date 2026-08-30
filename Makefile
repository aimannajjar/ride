.PHONY: clean debug perf_record perf_stat

NAME 				:= ride
BUILD_DIR		:= build
SRCS				:= $(wildcard src/*.c)
BPF_SRCS 		:= $(wildcard src/*.bpf.c)
BPF_OBJS		:= $(BPF_SRCS:src/%.c=$(BUILD_DIR)/%.o)
BPF_INCLUDE := -I${LINUX} -I${LIBBPF} -I./src
BPF_FLAGS		:= $(BPF_INCLUDE) -target bpf -g -O3 -std=gnu11 -c 
BPF_CC 			:= clang

all: $(BUILD_DIR)/$(NAME)

debug: clean $(BUILD_DIR)/$(NAME).skel.h 
	cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build build

perf_record: $(BUILD_DIR)/$(NAME)
	bash ./tests/0_perf_record.sh

perf_stat: $(BUILD_DIR)/$(NAME)
	bash ./tests/0_perf_stat.sh

$(BUILD_DIR)/$(NAME): $(SRCS) $(BUILD_DIR)/$(NAME).skel.h CMakeLists.txt | $(BUILD_DIR)
	cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build

$(BUILD_DIR)/$(NAME).skel.h: $(BPF_OBJS) | $(BUILD_DIR)
	bpftool gen skeleton $< > $@

$(BPF_OBJS): $(BPF_SRCS) | $(BUILD_DIR)
	$(BPF_CC) $(BPF_FLAGS) $< -o $@


$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf *.o $(NAME).skel.h build
