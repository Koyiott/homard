# HOMARD artifact - figure and table regeneration.
PY ?= python3
FIGDIR ?= figures

.PHONY: all quick figures tables check clean help

help:
	@echo "make figures   regenerate every figure and table (~5 min)"
	@echo "make quick     same, minus the Monte-Carlo step (~30 s)"
	@echo "make check     verify the environment and the data checksums"
	@echo "make clean     remove regenerated output (keeps data/ and reference/)"

all: figures

figures:
	$(PY) scripts/run_all.py

quick:
	$(PY) scripts/run_all.py --quick

tables:
	$(PY) scripts/run_all.py tab01 tab02

check:
	$(PY) scripts/check_env.py

clean:
	rm -rf $(FIGDIR)/_template_work
	find $(FIGDIR) -maxdepth 1 -type f \( -name '*.pdf' -o -name '*.png' -o -name '*.txt' -o -name '*.json' \) -delete
	@echo "cleaned $(FIGDIR)/ (reference/ and schematics/ untouched)"
