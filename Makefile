PYTHON ?= work/venv/bin/python
MODULE ?= rtld
THROUGH ?= nnMain
export PYTHONDONTWRITEBYTECODE := 1
export TMPDIR := $(CURDIR)/work/tmp

.PHONY: test test-core inspect generate check-c probe build load-only startup import-tools import-game
test-core:
	$(PYTHON) tools/test_core.py

test:
	mkdir -p work/tmp
	$(PYTHON) tools/bounded.py --log work/logs/tests-upstream.log -- $(PYTHON) -m unittest discover -s third_party/lasrecomp/tools -p 'test_*.py' -v
	$(PYTHON) tools/bounded.py --log work/logs/tests-project.log -- $(PYTHON) -m unittest discover -s tests -v

inspect generate check-c probe:
	@test -n "$(EXEFS)" || (echo 'Set EXEFS to your extracted v1.4.3 ExeFS directory'; exit 1)
	$(PYTHON) tools/recompile.py $@ --exefs "$(EXEFS)" --module "$(MODULE)"

build:
	@test -n "$(EXEFS)" || (echo 'Set EXEFS to your extracted v1.4.3 ExeFS directory'; exit 1)
	$(PYTHON) tools/build.py --exefs "$(EXEFS)"

load-only:
	@test -n "$(EXEFS)" || (echo 'Set EXEFS to your extracted v1.4.3 ExeFS directory'; exit 1)
	$(PYTHON) tools/load.py --exefs "$(EXEFS)"

startup:
	@test -n "$(EXEFS)" || (echo 'Set EXEFS to your extracted v1.4.3 ExeFS directory'; exit 1)
	$(PYTHON) tools/startup.py --exefs "$(EXEFS)" --through "$(THROUGH)" $(if $(ROMFS),--romfs "$(ROMFS)")

import-tools:
	mkdir -p work/tmp work/logs
	$(PYTHON) tools/bounded.py --log work/logs/import-tools.log -- tools/prepare_hactool.sh

import-game: import-tools
	@test -n "$(BASE)" -a -n "$(UPDATE)" -a -n "$(KEYS)" || (echo 'Set BASE, UPDATE and KEYS to your local files'; exit 1)
	$(PYTHON) tools/bounded.py --log work/logs/import-game.log -- $(PYTHON) tools/import_game.py --base "$(BASE)" --update "$(UPDATE)" --keys "$(KEYS)"
