CXX             ?= clang++
VERSION         = 0.8
DEFVER          = -DVERSION=$(VERSION)
CXXFLAGS        = -std=c++20 -O3 -Wall -Wextra -fno-strict-aliasing -fexceptions -fno-rtti $(DEFVER)

EMSDK_DIR       ?= $(HOME)/Work/me/emsdk/upstream/emscripten
EMCC            = $(shell which em++ 2>/dev/null || echo $(EMSDK_DIR)/em++)
EMFLAGS         = -std=c++20 -O3 -fno-strict-aliasing -fexceptions -fno-rtti $(DEFVER) \
                  -s WASM=1 \
                  -s MODULARIZE=1 \
                  -s EXPORT_NAME='createVxsModule' \
                  -s ENVIRONMENT='web,node' \
                  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8"]' \
                  -s EXPORTED_FUNCTIONS='["_vxs_init","_vxs_eval","_vxs_eval_json","_vxs_step_fibers","_vxs_active_fibers_count","_vxs_clear_fibers","_vxs_stats_json","_vxs_settle_number","_vxs_settle_string","_vxs_settle_error","_vxs_settle_handle","_vxs_pending_externals","_vxs_host_handle_count","_malloc","_free","_vxs_settle_handle"]' \
                  -s ALLOW_MEMORY_GROWTH=1 \
                  -s NO_EXIT_RUNTIME=1

NODE            = $(shell which node 2>/dev/null || echo $(EMSDK_DIR)/../../node/*/bin/node)

all: vx-scheme wasm

# Recipes below are @-silenced so a test run reads as results rather than
# as a transcript of the commands producing them. Compiler and harness
# errors still surface — those go to stderr, which is untouched. Use
# `make V=1` to see the commands.
V ?= 0
ifeq ($(V),0)
Q = @
else
Q =
endif

# Every lib/*.scm is compiled into the binary as data (see embed_libs.sh).
# The wasm build has no filesystem, so `load` cannot reach lib/ there, and
# the browser is exactly where a livecoding library is wanted. Nothing in
# the table is evaluated until something loads it — only prelude.scm runs
# at startup — so a library costs nothing until it is used. Kept as real
# Scheme on disk so it can be edited and diffed, not as escaped C++.
SCHEME_LIBS := $(wildcard ../lib/*.scm)

vx_embedded_libs.h: $(SCHEME_LIBS) embed_libs.sh
	@echo "=== EMBEDDING lib/*.scm ==="
	$(Q)./embed_libs.sh ../lib > $@

vx-scheme: vx_vm.cpp main.cpp vx_value.h vx_heap.h vx_vm.h vx_reader.h vx_compiler.h vx_embedded_libs.h
	@echo "=== BUILDING NATIVE vx-scheme ==="
	$(Q)$(CXX) $(CXXFLAGS) vx_vm.cpp main.cpp -o vx-scheme

wasm: vx_vm.cpp vx_wasm.cpp vx_value.h vx_heap.h vx_vm.h vx_reader.h vx_compiler.h vx_embedded_libs.h
	@echo "=== BUILDING WASM (web/vxs.js, web/vxs.wasm) ==="
	$(Q)mkdir -p ../web
	$(Q)$(EMCC) $(EMFLAGS) vx_vm.cpp vx_wasm.cpp -o ../web/vxs.js

test: vx-scheme wasm test-suite test-gc test-unit test-wasm test-ports test-async test-aot test-r4rs
	@echo ""
	@echo "════════════════════════════════════════════════════════════════"
	@echo "  ✨ ALL TEST PHASES PASSED ✨"
	@echo "════════════════════════════════════════════════════════════════"

test-suite: vx-scheme
	@echo "=== RUNNING GROUND-UP SCHEME TEST SUITE ==="
	$(Q)./vx-scheme ../testcases/run_suite.scm

# The original 2002-era regression harness: r4rstest.scm (SLIB's R4RS
# conformance suite) plus the classic benchmarks, each diffed
# byte-for-byte against testcases/good/. Exercises things the ground-up
# suite doesn't reach for free — dynamic.scm's self-hosted type
# inferencer parsing its own 2300-line source was what first surfaced
# the GC rooting bug fixed earlier this session, for instance. Was run
# by hand all session; wired in here so a regression here can't go
# unnoticed the way it did before.
test-r4rs: vx-scheme
	@echo "=== RUNNING CLASSIC vx-test.scm HARNESS (R4RS + benchmarks vs. golden files) ==="
	$(Q)cd ../testcases && ../src/vx-scheme vx-test.scm

test-gc:
	@echo "=== RUNNING MARK-AND-SWEEP GC UNIT & STRESS TESTS ==="
	$(Q)$(CXX) $(CXXFLAGS) vx_vm.cpp ../testcases/test_vx_gc.cpp -o test_gc && ./test_gc && rm test_gc

test-unit:
	@echo "=== RUNNING STRUCTURED 58-TEST UNIT MATRIX ==="
	$(Q)$(NODE) ../testcases/run_all_tests.js

test-wasm:
	@echo "=== RUNNING WASM PRESETS & FIBER SCHEDULER TESTS ==="
	$(Q)$(NODE) ../testcases/test_wasm_presets.js

# Browser output ports: where `display` actually lands in the wasm build.
# Needs a real module with JS sinks attached, so it cannot live in the
# Scheme suite — the whole question is which host callback receives the
# text.
test-ports:
	@echo "=== RUNNING BROWSER HOST TESTS (output ports, form sequencing) ==="
	$(Q)$(NODE) ../testcases/test_browser_ports.js

# Externally-settled futures. Necessarily lives here rather than in the
# Scheme suite: the point is that a fiber blocks until the JS EVENT LOOP
# settles it, which needs a host with an event loop and a driver that
# returns control between steps. setTimeout stands in for the
# promise-returning WebGPU calls this exists for.
test-async:
	@echo "=== RUNNING ASYNC / EXTERNAL FUTURE TESTS ==="
	$(Q)$(NODE) ../testcases/test_async_futures.js

# AOT: compile a program to a standalone native binary and check what it
# prints against the same golden file the interpreter is held to. Used to
# run the benchmark programs too and just dump their output — but
# test-r4rs already runs every one of them diffed against good/, so that
# was noise verifying nothing. This keeps the part that was actually
# unique: that the AOT path agrees with the interpreter.
test-aot: vx-scheme
	@echo "=== VERIFYING STANDALONE AOT NATIVE COMPILER ==="
	@./vx-scheme -c ../testcases/ack.scm -o ack_bin > /dev/null
	@./ack_bin > ack_aot.out
	@if diff -q ack_aot.out ../testcases/good/ack.good > /dev/null 2>&1; then \
	  echo "  ✅ [PASS] AOT binary output matches good/ack.good"; \
	  rm -f ack_bin ack_aot.out; \
	else \
	  echo "  ❌ [FAIL] AOT binary output differs from good/ack.good"; \
	  diff ack_aot.out ../testcases/good/ack.good || true; \
	  rm -f ack_bin ack_aot.out; \
	  exit 1; \
	fi

# AddressSanitizer. Deliberately NOT part of `test` — it needs its own -O0
# build and runs several times slower, so it belongs before committing VM
# work rather than in the edit loop.
#
# It has already paid for itself: the use-after-free in touch presented as
# a bare segfault, and ASan turned that into three stack traces naming the
# allocation, the free and the read. That is the difference between "the
# scheduler deletes fibers and touch holds a raw pointer" as a hypothesis
# and as a fact.
asan: vx_vm.cpp main.cpp vx_value.h vx_heap.h vx_vm.h vx_reader.h vx_compiler.h vx_embedded_libs.h
	@echo "=== BUILDING + RUNNING SUITE UNDER ADDRESSSANITIZER ==="
	$(Q)$(CXX) -std=c++20 -O0 -g -fsanitize=address -fno-omit-frame-pointer \
	    -fno-strict-aliasing -fexceptions -fno-rtti $(DEFVER) \
	    vx_vm.cpp main.cpp -o vx-scheme-asan
	$(Q)cd .. && src/vx-scheme-asan testcases/run_suite.scm
	$(Q)cd ../testcases && ../src/vx-scheme-asan vx-test.scm
	@rm -f vx-scheme-asan
	@echo "  ✅ [PASS] no ASan reports"

# Deliberately NOT part of `test`: these print maze art, series tables and
# pi digits, and check nothing. test-r4rs runs the same programs against
# golden files and reports a total time. This target is for when you
# actually want to look at the output.
bench: vx-scheme
	@echo "=== CLASSIC SCHEME BENCHMARKS (visual output, unverified) ==="
	./vx-scheme ../testcases/ack.scm
	./vx-scheme ../testcases/puzzle.scm
	./vx-scheme ../testcases/maze.scm
	./vx-scheme ../testcases/pi.scm
	./vx-scheme ../testcases/sieve.scm
	./vx-scheme ../testcases/series.scm

clean:
	rm -f *.o vx_embedded_libs.h vx-scheme vx-scheme-asan vxs-* test_fibers ack_bin ack_aot.out a.out
	rm -rf *.dSYM
