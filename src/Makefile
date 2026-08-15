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

all: vx-scheme wasm

vx-scheme: vx_vm.cpp main.cpp vx_value.h vx_heap.h vx_vm.h vx_reader.h vx_compiler.h
	$(CXX) $(CXXFLAGS) vx_vm.cpp main.cpp -o vx-scheme

wasm: vx_vm.cpp vx_wasm.cpp vx_value.h vx_heap.h vx_vm.h vx_reader.h vx_compiler.h
	mkdir -p ../web
	$(EMCC) $(EMFLAGS) vx_vm.cpp vx_wasm.cpp -o ../web/vxs.js

test: vx-scheme wasm
	$(EMSDK_DIR)/../../node/*/bin/node ../testcases/run_all_tests.js
	$(EMSDK_DIR)/../../node/*/bin/node ../testcases/test_wasm_presets.js
	./vx-scheme -e '(let [v [4 5 6] a {:x 10 :y 20}] [(v 1) (:y a)])'

clean:
	rm -f *.o vx-scheme vxs-* test_fibers
	rm -rf *.dSYM
