CXX             ?= clang++
VERSION         = 0.8
DEFVER          = -DVERSION=$(VERSION)
CXXFLAGS        = -std=c++20 -O3 -Wall -Wextra -fno-strict-aliasing -fexceptions -fno-rtti $(DEFVER)

EMSDK_DIR       ?= $(HOME)/Work/me/emsdk/upstream/emscripten
EMCC            = $(shell which em++ 2>/dev/null || echo $(EMSDK_DIR)/em++)
EMFLAGS         = -std=c++20 -O3 -fno-strict-aliasing -fexceptions -fno-rtti $(DEFVER) \
                  -s WASM=1 \
                  -s MODULARIZE=1 \
                  -s EXPORT_NAME='createVxSchemeModule' \
                  -s ENVIRONMENT='web,node' \
                  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8"]' \
                  -s EXPORTED_FUNCTIONS='["_vxs_init","_vxs_eval","_vxs_eval_json","_vxs_step_fibers","_vxs_active_fibers_count","_vxs_clear_fibers","_malloc","_free"]' \
                  -s ALLOW_MEMORY_GROWTH=1 \
                  -s NO_EXIT_RUNTIME=1

NODE            = $(shell which node 2>/dev/null || echo $(EMSDK_DIR)/../../node/*/bin/node)

all: vx-scheme wasm

vx-scheme: vx_vm.cpp main.cpp vx_value.h vx_heap.h vx_vm.h vx_reader.h vx_compiler.h
	$(CXX) $(CXXFLAGS) vx_vm.cpp main.cpp -o vx-scheme

wasm: vx_vm.cpp vx_wasm.cpp vx_value.h vx_heap.h vx_vm.h vx_reader.h vx_compiler.h
	mkdir -p ../web
	$(EMCC) $(EMFLAGS) vx_vm.cpp vx_wasm.cpp -o ../web/vxs.js

test: vx-scheme wasm test-suite test-gc test-unit test-wasm test-bench

test-suite: vx-scheme
	@echo "=== RUNNING GROUND-UP SCHEME TEST SUITE ==="
	./vx-scheme ../testcases/run_suite.scm

test-gc:
	@echo "=== RUNNING MARK-AND-SWEEP GC UNIT & STRESS TESTS ==="
	$(CXX) $(CXXFLAGS) vx_vm.cpp ../testcases/test_vx_gc.cpp -o test_gc && ./test_gc && rm test_gc

test-unit:
	@echo "=== RUNNING STRUCTURED 58-TEST UNIT MATRIX ==="
	$(NODE) ../testcases/run_all_tests.js

test-wasm:
	@echo "=== RUNNING WASM PRESETS & FIBER SCHEDULER TESTS ==="
	$(NODE) ../testcases/test_wasm_presets.js

test-bench: vx-scheme
	@echo "=== RUNNING CLASSIC SCHEME BENCHMARKS ==="
	./vx-scheme ../testcases/ack.scm
	# ./vx-scheme ../testcases/puzzle.scm  # Paused pending continuation refactoring
	./vx-scheme ../testcases/pi.scm
	./vx-scheme ../testcases/sieve.scm
	./vx-scheme ../testcases/series.scm
	@echo "=== VERIFYING STANDALONE AOT NATIVE COMPILER ==="
	./vx-scheme -c ../testcases/ack.scm -o ack_bin && ./ack_bin && rm ack_bin

clean:
	rm -f *.o vx-scheme vxs-* test_fibers ack_bin a.out
	rm -rf *.dSYM
