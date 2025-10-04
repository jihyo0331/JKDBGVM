# Makefile - snapctl + optional snapctl-gtk build (GTK source excluded from default build)

CC        ?= cc
CFLAGS    ?= -O2 -g -Wall
OUT_DIR   ?= ./build
BIN_NAME  ?= snapctl
BIN       := $(OUT_DIR)/$(BIN_NAME)

# GTK GUI source path (we'll exclude this from default SRCS)
GTK_SRC   := snapctl/snapctl-gtk.c
GTK_BIN   := $(OUT_DIR)/snapctl-gtk
GTK_PKG   := gtk+-3.0
GTK_CFLAGS:= $(shell pkg-config --cflags $(GTK_PKG) 2>/dev/null)
GTK_LIBS  := $(shell pkg-config --libs   $(GTK_PKG) 2>/dev/null)

# All C files in snapctl/, but filter out GTK_SRC so default build doesn't require GTK dev
ALL_SNAPCTL_C := $(wildcard snapctl/*.c)
SRCS := $(filter-out $(GTK_SRC), $(ALL_SNAPCTL_C))

# Default QMP socket (can override when running)
SOCKET_DEFAULT := $(HOME)/vm/win11/qmp.sock
SOCKET_FILE    := $(SOCKET_DEFAULT)

.PHONY: all default gtk gui run run-gui demo clean help socket check-socket

.DEFAULT_GOAL := all

default: all
all: $(BIN) $(GTK_BIN)

$(OUT_DIR):
	@mkdir -p $(OUT_DIR)

# Build console binary from SRCS (GTK source excluded)
$(BIN): $(SRCS) | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRCS)
	@echo "Built console binary: $@"

# Ensure QMP socket 경로를 확인 (실제 소켓은 VM이 생성해야 함)
check-socket:
	@mkdir -p $(dir $(SOCKET_FILE))
	@if [ -S "$(SOCKET_FILE)" ]; then \
	  echo "QMP 소켓 확인: $(SOCKET_FILE)"; \
	else \
	  echo "ERROR: QMP 소켓이 없습니다: $(SOCKET_FILE)"; \
	  echo "       먼저 VM을 실행해서 QMP 소켓을 생성하세요."; \
	  exit 1; \
	fi

# GTK GUI build (only when explicitly requested)
$(GTK_BIN): $(GTK_SRC) $(BIN) | $(OUT_DIR)
	@if [ -z "$(GTK_LIBS)" ]; then \
	  echo "ERROR: GTK3 dev 패키지가 필요합니다 (예: libgtk-3-dev / gtk3-devel)."; \
	  echo "       pkg-config가 '$(GTK_PKG)'를 찾지 못했습니다."; \
	  echo "       On Fedora: sudo dnf install gtk3-devel pkgconfig"; \
	  exit 1; \
	fi
	$(CC) $(CFLAGS) $(GTK_CFLAGS) \
	  -DSNAPCTL=\"$(abspath $(BIN))\" \
	  -DSOCKET_PATH=\"$(SOCKET_DEFAULT)\" \
	  -o $@ $(GTK_SRC) $(GTK_LIBS)
	@echo "Built GTK GUI binary: $@"

gtk: $(GTK_BIN)
gui: gtk
socket: check-socket

# run / demo commands (useful targets)
run: $(BIN) check-socket
	@echo "Running: $(BIN) --socket \"$(HOME)/vm/win11/qmp.sock\" list"
	@$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" list

demo: $(BIN) check-socket
	@$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" list || true
	@$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" savevm snap1 || true
	@$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" loadvm snap1 || true
	@$(BIN) --socket "$(HOME)/vm/win11/qmp.sock" delvm  snap1 || true

run-gui: gtk check-socket
	@SOCKET="$(SOCKET)"; \
	if [ -z "$$SOCKET" ]; then SOCKET="$(HOME)/vm/win11/qmp.sock"; fi; \
	echo "Running GUI with socket: $$SOCKET"; \
	$(GTK_BIN) --socket "$$SOCKET"

clean:
	@echo "Cleaning $(OUT_DIR)"
	@rm -rf $(OUT_DIR)

help:
	@echo "Makefile targets:"
	@echo "  make         -> build console + gtk GUI (if GTK dev installed)"
	@echo "  make all     -> same as 'make'"
	@echo "  make gtk|gui -> build GTK GUI (requires gtk+-3.0 dev)"
	@echo "  make socket  -> QMP 소켓 존재 여부 확인 (실패 시 안내)"
	@echo "  make run     -> run console list command"
	@echo "  make demo    -> run demo sequence (list/save/load/del)"
	@echo "  make run-gui -> run the GTK GUI (override SOCKET=... to change socket)"
	@echo "  make clean   -> remove out dir"
