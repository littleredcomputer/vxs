all:
	$(MAKE) -C src

test:
	$(MAKE) -C src test

wasm:
	$(MAKE) -C src wasm

clean:
	$(MAKE) -C src clean

.PHONY: all test wasm clean
