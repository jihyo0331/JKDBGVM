.PHONY: all clean run demo

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wshadow
LDFLAGS ?=
LDLIBS  ?=
LDLIBS  += -lm

OUT_DIR := build
OBJ_DIR := $(OUT_DIR)/obj
SRC     := snapctl/snapctl.c snapctl/time.c 
BIN     := $(OUT_DIR)/snapctl


all: $(BIN)

$(OUT_DIR):
	@mkdir -p $(OUT_DIR)

OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRC))

$(OBJ_DIR): | $(OUT_DIR)
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)
	@echo "Built $@"

run: all
	./$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" list

demo: all
	./$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" list || true
	./$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" savevm snap1 || true
	./$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" loadvm snap1 || true
	./$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" delvm  snap1 || true

# ---- GTK GUI (snapctl-gtk) ---- (붙여넣음)
GTK_SRC   := snapctl/snapctl-gtk.c
GTK_BIN   := $(OUT_DIR)/snapctl-gtk
GTK_PKG   := gtk+-3.0
GTK_CFLAGS:= $(shell pkg-config --cflags $(GTK_PKG) 2>/dev/null)
GTK_LIBS  := $(shell pkg-config --libs   $(GTK_PKG) 2>/dev/null)

# GUI 빌드 (pkg-config가 gtk를 찾을 수 있어야 함)
gtk: $(GTK_BIN)
gui: gtk  ## 별칭

$(GTK_BIN): $(GTK_SRC) $(BIN) | $(OUT_DIR)
	@if [ -z "$(GTK_LIBS)" ]; then \
	  echo "ERROR: GTK3 dev 패키지가 필요합니다 (libgtk-3-dev / gtk3-devel)."; \
	  echo "       pkg-config가 '$(GTK_PKG)'를 찾지 못했습니다."; \
	  exit 1; \
	fi
	$(CC) $(CFLAGS) $(GTK_CFLAGS) \
	  -DSNAPCTL=\"$(abspath $(BIN))\" \
	  -DSOCKET_PATH=\"$(HOME)/vm/win11/qmp.sock\" \
	  -o $@ $< $(GTK_LIBS)
	@echo "Built $@"

# GUI 실행(소켓 경로 바꾸고 싶으면 SOCKET=... 로 override 가능)
run-gui: gtk
	@SOCKET="$(SOCKET)"; \
	if [ -z "$$SOCKET" ]; then SOCKET="$(HOME)/vm/win11/qmp.sock"; fi; \
	echo "Running GUI with socket: $$SOCKET"; \
	$(GTK_BIN)

clean:
	@rm -rf $(OUT_DIR)
