# Makefile for ngx_http_upstream_healthcheck_module
#
# nginx modules are compiled through nginx's own configure script, so this
# Makefile automates the whole flow: fetch the nginx source, configure it
# with the module and build.
#
# Typical usage:
#   make                # build the dynamic module (.so) and copy it to dist/
#   make test           # end-to-end test suite against dist/'s .so
#   make clean          # clean nginx source build objects and test certs
#   make distclean      # remove build/ and dist/ entirely
#
# Version and configure options are auto-detected from `nginx -V` (the
# nginx already installed on the system, however it got there: apt, dnf,
# brew, a manual build, a Docker image), so the same command works
# identically on Ubuntu, RHEL, macOS or in a container without having to
# guess the version or compiled-in features by hand. The nginx source is
# still required to build (no packaging system ships nginx's internal
# headers as a separate package: that's not a limitation of this Makefile,
# it's true for any nginx module, third-party or official) and is
# downloaded once into build/ — subsequent builds reuse it without
# touching the network.
#
# Overridable variables:
#   make NGINX_BIN=/usr/sbin/nginx             # which installed nginx to detect
#   make NGINX_VERSION=1.27.4                  # force the version (skip detection)
#   make CONFIGURE_OPTS="--with-http_ssl_module"   # force the options (skip detection)
#   make NGINX_SRC=/path/to/nginx/source       # use an already-present source
#                                               # (e.g. an existing from-source
#                                               # install): nothing gets downloaded

# Directory this Makefile lives in (not necessarily the cwd "make" was
# invoked from), so its own scripts can be found regardless of where the
# build is launched from.
MK_DIR                 := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

NGINX_BIN              ?= nginx
NGINX_DETECTED_VERSION := $(shell $(NGINX_BIN) -v 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)

# Options detected from "nginx -V", filtered by detect-nginx-opts.sh to keep
# only the ones vanilla nginx.org's ./configure recognizes: distros (e.g.
# Debian/Ubuntu) often patch their own configure with extra packaging
# options (--override-system=, a --build= used in non-standard ways, etc.)
# that the official configure doesn't understand and would fail on with
# "invalid option" if replayed verbatim.
NGINX_DETECTED_ARGS    := $(shell $(MK_DIR)detect-nginx-opts.sh $(NGINX_BIN))

NGINX_VERSION  ?= $(if $(NGINX_DETECTED_VERSION),$(NGINX_DETECTED_VERSION),1.26.2)
NGINX_TARBALL  := nginx-$(NGINX_VERSION).tar.gz
NGINX_URL      := https://nginx.org/download/$(NGINX_TARBALL)
NGINX_URL_GH   := https://github.com/nginx/nginx/archive/refs/tags/release-$(NGINX_VERSION).tar.gz

MODULE_DIR     := $(abspath .)
BUILD_DIR      := $(MODULE_DIR)/build
DIST_DIR       := $(MODULE_DIR)/dist
NGINX_SRC      ?= $(BUILD_DIR)/nginx-$(NGINX_VERSION)

# The configure script is called ./configure in the official tarball and
# auto/configure in the GitHub tree: handle both.
CONFIGURE       = $(shell test -x $(NGINX_SRC)/configure && echo ./configure || echo auto/configure)

# The same options the installed nginx was built with (detected from
# "nginx -V"), plus --with-compat which guarantees the module is loadable
# even if it doesn't replicate every single option exactly. If nginx isn't
# found in PATH, a reasonable minimal fallback (CC_OPT is included here
# because, when options are detected, their own --with-cc-opt replaces
# ours instead of merging with it: nginx only keeps the last occurrence of
# a repeated option. It isn't actually needed in that case: nginx's default
# CFLAGS already include -Wno-unused-parameter).
CC_OPT         ?= -Wno-unused-parameter
CONFIGURE_OPTS ?= $(if $(NGINX_DETECTED_ARGS),$(NGINX_DETECTED_ARGS) --with-compat,--with-http_ssl_module --with-cc-opt="$(CC_OPT)" --with-compat)

MODULE_SRCS    := config \
                  include/ngx_http_upstream_healthcheck_module.h \
                  src/ngx_http_upstream_healthcheck_module.c \
                  src/ngx_http_hc_config.c \
                  src/ngx_http_hc_upstream.c \
                  src/ngx_http_hc_balancer.c \
                  src/ngx_http_hc_probe.c \
                  src/ngx_http_hc_resolve.c \
                  src/ngx_http_hc_status.c

.PHONY: all dynamic test clean distclean help

all: dynamic

# ---------------------------------------------------------- nginx source

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(NGINX_SRC)/.downloaded: | $(BUILD_DIR)
	@if [ ! -d "$(NGINX_SRC)" ]; then \
	    echo ">> Fetching nginx $(NGINX_VERSION)"; \
	    ( curl -fsSL -o $(BUILD_DIR)/$(NGINX_TARBALL) $(NGINX_URL) \
	      && tar -xzf $(BUILD_DIR)/$(NGINX_TARBALL) -C $(BUILD_DIR) ) \
	    || ( echo ">> nginx.org unreachable, trying GitHub"; \
	         curl -fsSL -o $(BUILD_DIR)/$(NGINX_TARBALL) $(NGINX_URL_GH) \
	         && tar -xzf $(BUILD_DIR)/$(NGINX_TARBALL) -C $(BUILD_DIR) \
	         && mv $(BUILD_DIR)/nginx-release-$(NGINX_VERSION) $(NGINX_SRC) ); \
	fi
	touch $@

# -------------------------------------------------------- dynamic build

dynamic: $(NGINX_SRC)/.downloaded $(MODULE_SRCS)
	cd $(NGINX_SRC) && $(CONFIGURE) \
	    --add-dynamic-module=$(MODULE_DIR) \
	    $(CONFIGURE_OPTS)
	$(MAKE) -C $(NGINX_SRC) modules
	mkdir -p $(DIST_DIR)
	cp $(NGINX_SRC)/objs/ngx_http_upstream_healthcheck_module.so $(DIST_DIR)/
	@echo
	@echo ">> Module: $(DIST_DIR)/ngx_http_upstream_healthcheck_module.so"
	@echo ">> Load it in nginx.conf with:"
	@echo ">>   load_module $(DIST_DIR)/ngx_http_upstream_healthcheck_module.so;"

# ---------------------------------------------------------- smoke test

TEST_DIR := $(MODULE_DIR)/test

test: dynamic
	@if [ ! -f $(TEST_DIR)/cert.pem ] || [ ! -f $(TEST_DIR)/key.pem ]; then \
	    echo ">> Generating test TLS certificate ($(TEST_DIR)/cert.pem, key.pem)"; \
	    openssl req -x509 -newkey rsa:2048 \
	        -keyout $(TEST_DIR)/key.pem -out $(TEST_DIR)/cert.pem \
	        -days 30 -nodes -subj "/CN=backend.internal" 2>/dev/null; \
	fi
	python3 $(TEST_DIR)/test_healthcheck.py

# --------------------------------------------------------------- clean

clean:
	-$(MAKE) -C $(NGINX_SRC) clean 2>/dev/null || true
	rm -f $(TEST_DIR)/cert.pem $(TEST_DIR)/key.pem
	rm -rf $(TEST_DIR)/__pycache__

distclean: clean
	rm -rf $(BUILD_DIR) $(DIST_DIR)

help:
	@echo "Available targets:"
	@echo "  make [dynamic]  build the dynamic module (.so) and copy it to dist/"
	@echo "  make test       end-to-end test suite (requires python3 and openssl)"
	@echo "  make clean      clean nginx source build objects and test certs"
	@echo "  make distclean  remove build/ and dist/ entirely"
	@echo
	@echo "Variables: NGINX_VERSION, NGINX_SRC, CONFIGURE_OPTS, CC_OPT"
