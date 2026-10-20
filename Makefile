PYTHON ?= work/venv/bin/python
MODULE ?= rtld
export PYTHONDONTWRITEBYTECODE := 1
export TMPDIR := $(CURDIR)/work/tmp

.PHONY: test inspect generate check-c probe
test:
	mkdir -p work/tmp
	$(PYTHON) -m unittest discover -s third_party/lasrecomp/tools -p 'test_*.py' -v
	$(PYTHON) -m unittest discover -s tests -v

inspect generate check-c probe:
	@test -n "$(EXEFS)" || (echo 'Set EXEFS to your extracted v1.4.3 ExeFS directory'; exit 1)
	$(PYTHON) tools/recompile.py $@ --exefs "$(EXEFS)" --module "$(MODULE)"
