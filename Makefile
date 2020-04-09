all:
	@$(MAKE) -C module

.phony: clean

clean:
	@$(MAKE) -C module clean
