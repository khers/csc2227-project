all:
	@$(MAKE) -C module $(MAKE_OPTS) build
	@$(MAKE) -C lib $(MAKE_OPTS) all
	@$(MAKE) -C test $(MAKE_OPTS) all

.PHONY : clean
clean:
	@$(MAKE) -C module $(MAKE_OPTS) clean
	@$(MAKE) -C lib $(MAKE_OPTS) clean
	@$(MAKE) -C test $(MAKE_OPTS) clean
