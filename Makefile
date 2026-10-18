# 3DSLink
#   make          build build/app/3DSLink.cia (includes the streaming service)
#   make host     build the PC test harness
#   make test     run the browser end-to-end test against the harness
#   make art      redraw the HOME Menu icon and banner

all:
	$(MAKE) -C app

service:
	$(MAKE) -C service

host:
	$(MAKE) -C host

test: host
	node host/test/e2e.mjs

art:
	python3 tools/make_art.py

clean:
	rm -rf build

.PHONY: all service host test art clean
