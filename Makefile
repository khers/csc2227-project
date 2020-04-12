all:
	@$(MAKE) -C module $(MAKE_OPTS) build
	@$(MAKE) -C lib $(MAKE_OPTS) all

.phony: clean

clean:
	@$(MAKE) -C module $(MAKE_OPTS) clean
	@$(MAKE) -C lib $(MAKE_OPTS) clean
