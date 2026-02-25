# Makefile (Root)
# version 1.0.0
# date 2026-02-25
# author Leonardo Lisa
# brief Top-level Makefile to build all RPi5 Fast IRQ components.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

SUBDIRS = kernel_module Basic_usage Benchmark CountsPerSecond CountsPerSecond_Plot

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done