PYTHON ?= work/venv/bin/python
MODULE ?= rtld
export PYTHONDONTWRITEBYTECODE := 1
export TMPDIR := $(CURDIR)/work/tmp

.PHONY: test inspect generate check-c probe build load-only
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
