# hwscan (C++) -- static, dependency-free hardware inventory scanner.
#
#   make            build ./hwscan (dynamically linked, for development)
#   make static     build a fully static binary for the boot image
#   make test       run against a synthetic machine, no hardware needed
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:.cpp=.o)
BIN      := hwscan

.PHONY: all static test clean fixture

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# -static so the image needs no libstdc++/libc at all; -s strips symbols.
static: CXXFLAGS += -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections
static: clean
	$(CXX) $(CXXFLAGS) -static -s -Wl,--gc-sections -o $(BIN) $(SRC)
	@echo; ls -lh $(BIN) | awk '{print "static binary: " $$5}'

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

fixture:
	@python3 tools/make_fixture.py /tmp/hwscan-fixture

test: $(BIN) fixture
	@rm -rf /tmp/hwscan-out && mkdir -p /tmp/hwscan-out
	@./$(BIN) --sysroot /tmp/hwscan-fixture --output-dir /tmp/hwscan-out; \
	  rc=$$?; echo; echo "exit code: $$rc (0=PASS 1=WARN 2=FAIL 3=error)"
	@echo; echo "--- generated row ---"
	@python3 -c "import csv;r=list(csv.DictReader(open('/tmp/hwscan-out/inventory.csv',encoding='utf-8-sig')))[-1];[print('%-26s | %s'%(k,v)) for k,v in r.items()]"

clean:
	rm -f $(OBJ) $(BIN)
