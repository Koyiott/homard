# Makefile for JestINA — Jetson TX2 (Tegra186 / Cortex-A57, LPDDR4)
#
# On-board compilation (run directly on the Jetson TX2):
#   make all
#
# Cross-compilation (from x86_64 host):
#   make CROSS=aarch64-linux-gnu- all

CROSS ?=
CC = $(CROSS)gcc
CFLAGS = -O2 -march=armv8-a -mtune=cortex-a57 -std=gnu11 -Wall
LDFLAGS = -lm -lpthread -lrt

BUILD_DIR = build
RESULTS_DIR = results

TOOLS = tx2_ina_randpair tx2_dram_mapper tx2_ina_burst tx2_ina_burst_heavy tx2_burst_pa tx2_burst_multi tx2_targeted tx2_knockknock_full tx2_chipselect tx2_sudoku tx2_sudoku_rowcol tx2_sudoku_refresh tx2_sudoku_tRDRD tx2_va_oracle tx2_va_oracle_4k tx2_va_oracle_nvmap tx2_nvmap_probe tx2_nvmap_contig tx2_drama

.PHONY: all tools clean dirs help run analyze

all: dirs tools
	@echo ""
	@echo "Build complete. Run on Jetson TX2:"
	@echo "  ./build/tx2_ina_randpair -n 500 -a 200000 -r 3 -o results/randpair.csv"
	@echo ""
	@echo "Then analyze:"
	@echo "  python3 analyze_randpair.py results/randpair.csv"

dirs:
	@mkdir -p $(BUILD_DIR) $(RESULTS_DIR)

tools: $(addprefix $(BUILD_DIR)/,$(TOOLS))

$(BUILD_DIR)/%: %.c | dirs
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: dirs tools
	./$(BUILD_DIR)/tx2_ina_randpair -n 500 -a 200000 -r 3 -o $(RESULTS_DIR)/randpair.csv

analyze:
	python3 analyze_randpair.py $(RESULTS_DIR)/randpair.csv

run-mapper: dirs tools
	sudo ./$(BUILD_DIR)/tx2_dram_mapper -o $(RESULTS_DIR)/dram_map

extract:
	python3 extract_mapping.py $(RESULTS_DIR)/dram_map_conflictset.csv

sudoku-full: dirs tools
	sudo bash run_sudoku_full.sh

sudoku-build: dirs $(BUILD_DIR)/tx2_sudoku_rowcol $(BUILD_DIR)/tx2_sudoku_refresh $(BUILD_DIR)/tx2_sudoku_tRDRD

clean:
	rm -rf $(BUILD_DIR)
	@echo "Build artifacts removed. Results preserved in $(RESULTS_DIR)/"

distclean: clean
	rm -rf $(RESULTS_DIR)

help:
	@echo "JestINA — DDR Row Conflict Detection via INA3221 on Jetson TX2"
	@echo ""
	@echo "Targets:"
	@echo "  all         Build all tools"
	@echo "  run         Run random-pair experiment (no root)"
	@echo "  analyze     Analyze random-pair results"
	@echo "  run-mapper  Run DRAM bank mapper (sudo required)"
	@echo "  extract     Extract bank functions from mapper output"
	@echo "  clean       Remove build artifacts"
	@echo "  distclean   Remove build + results"
