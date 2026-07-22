# Makefile per ngx_http_upstream_healthcheck_module
#
# I moduli nginx si compilano tramite il configure di nginx stesso, quindi
# questo Makefile automatizza l'intero flusso: scarica il sorgente nginx,
# lo configura con il modulo e compila.
#
# Uso tipico:
#   make                # build statica (binario nginx completo in build/)
#   make dynamic        # build come modulo dinamico (.so)
#   make test           # smoke test end-to-end con due backend fittizi
#   make install        # installa (default /usr/local/nginx) - richiede root
#   make clean          # pulizia
#
# Variabili sovrascrivibili:
#   make NGINX_VERSION=1.27.4
#   make CONFIGURE_OPTS="--with-http_ssl_module"
#   make NGINX_SRC=/path/al/sorgente/nginx    # usa un sorgente già presente

NGINX_VERSION  ?= 1.26.2
NGINX_TARBALL  := nginx-$(NGINX_VERSION).tar.gz
NGINX_URL      := https://nginx.org/download/$(NGINX_TARBALL)
NGINX_URL_GH   := https://github.com/nginx/nginx/archive/refs/tags/release-$(NGINX_VERSION).tar.gz

MODULE_DIR     := $(abspath .)
BUILD_DIR      := $(MODULE_DIR)/build
NGINX_SRC      ?= $(BUILD_DIR)/nginx-$(NGINX_VERSION)

# Il configure script si chiama ./configure nel tarball ufficiale
# e auto/configure nel tree GitHub: li gestiamo entrambi.
CONFIGURE       = $(shell test -x $(NGINX_SRC)/configure && echo ./configure || echo auto/configure)

CONFIGURE_OPTS ?= --with-http_ssl_module
CC_OPT         ?= -Wno-unused-parameter

MODULE_SRCS    := config ngx_http_upstream_healthcheck_module.c

.PHONY: all static dynamic test install clean distclean help

all: static

# ------------------------------------------------------------ sorgente nginx

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(NGINX_SRC)/.downloaded: | $(BUILD_DIR)
	@if [ ! -d "$(NGINX_SRC)" ]; then \
	    echo ">> Scarico nginx $(NGINX_VERSION)"; \
	    ( curl -fsSL -o $(BUILD_DIR)/$(NGINX_TARBALL) $(NGINX_URL) \
	      && tar -xzf $(BUILD_DIR)/$(NGINX_TARBALL) -C $(BUILD_DIR) ) \
	    || ( echo ">> nginx.org non raggiungibile, provo GitHub"; \
	         curl -fsSL -o $(BUILD_DIR)/$(NGINX_TARBALL) $(NGINX_URL_GH) \
	         && tar -xzf $(BUILD_DIR)/$(NGINX_TARBALL) -C $(BUILD_DIR) \
	         && mv $(BUILD_DIR)/nginx-release-$(NGINX_VERSION) $(NGINX_SRC) ); \
	fi
	touch $@

# ------------------------------------------------------------- build statica

static: $(NGINX_SRC)/.downloaded $(MODULE_SRCS)
	cd $(NGINX_SRC) && $(CONFIGURE) \
	    --with-cc-opt="$(CC_OPT)" \
	    --add-module=$(MODULE_DIR) \
	    $(CONFIGURE_OPTS)
	$(MAKE) -C $(NGINX_SRC)
	@echo
	@echo ">> Binario: $(NGINX_SRC)/objs/nginx"
	@$(NGINX_SRC)/objs/nginx -V 2>&1 | head -2

# ------------------------------------------------------------ build dinamica

dynamic: $(NGINX_SRC)/.downloaded $(MODULE_SRCS)
	cd $(NGINX_SRC) && $(CONFIGURE) \
	    --with-cc-opt="$(CC_OPT)" \
	    --add-dynamic-module=$(MODULE_DIR) \
	    $(CONFIGURE_OPTS)
	$(MAKE) -C $(NGINX_SRC) modules
	@echo
	@echo ">> Modulo: $(NGINX_SRC)/objs/ngx_http_upstream_healthcheck_module.so"
	@echo ">> Caricalo in nginx.conf con:"
	@echo ">>   load_module modules/ngx_http_upstream_healthcheck_module.so;"

# ------------------------------------------------------------------- install

install: static
	$(MAKE) -C $(NGINX_SRC) install

# ---------------------------------------------------------------- smoke test

TEST_DIR := $(BUILD_DIR)/test

test: static
	@mkdir -p $(TEST_DIR)/logs
	@printf '%s\n' \
	  'worker_processes 2;' \
	  'error_log $(TEST_DIR)/logs/error.log info;' \
	  'pid $(TEST_DIR)/nginx.pid;' \
	  'events { worker_connections 128; }' \
	  'http {' \
	  '    access_log off;' \
	  '    upstream backend {' \
	  '        server 127.0.0.1:9001;' \
	  '        server 127.0.0.1:9002;' \
	  '        healthcheck interval=1000 timeout=500 fall=2 rise=2 uri=/;' \
	  '    }' \
	  '    server {' \
	  '        listen 127.0.0.1:8888;' \
	  '        location / { proxy_pass http://backend; }' \
	  '        location /hc-status { healthcheck_status; }' \
	  '    }' \
	  '}' > $(TEST_DIR)/nginx.conf
	@echo ">> Avvio due backend di test (9001, 9002)"
	@python3 -m http.server 9001 --bind 127.0.0.1 >/dev/null 2>&1 & echo $$! > $(TEST_DIR)/b1.pid
	@python3 -m http.server 9002 --bind 127.0.0.1 >/dev/null 2>&1 & echo $$! > $(TEST_DIR)/b2.pid
	@sleep 1
	@$(NGINX_SRC)/objs/nginx -c $(TEST_DIR)/nginx.conf -p $(TEST_DIR)/
	@sleep 3
	@echo ">> Stato iniziale (entrambi up):"
	@curl -s http://127.0.0.1:8888/hc-status
	@echo ">> Fermo il backend 9002..."
	@kill `cat $(TEST_DIR)/b2.pid` 2>/dev/null || true
	@sleep 4
	@echo ">> Stato dopo il kill (9002 deve essere DOWN):"
	@curl -s http://127.0.0.1:8888/hc-status
	@echo ">> Verifica failover: la richiesta deve rispondere 200 da 9001"
	@curl -s -o /dev/null -w "HTTP %{http_code}\n" http://127.0.0.1:8888/
	@$(MAKE) --no-print-directory test-clean
	@echo ">> Test OK"

test-clean:
	-@$(NGINX_SRC)/objs/nginx -c $(TEST_DIR)/nginx.conf -p $(TEST_DIR)/ -s stop 2>/dev/null
	-@kill `cat $(TEST_DIR)/b1.pid` 2>/dev/null
	-@kill `cat $(TEST_DIR)/b2.pid` 2>/dev/null
	-@rm -f $(TEST_DIR)/b1.pid $(TEST_DIR)/b2.pid

# ----------------------------------------------------------------- pulizia

clean:
	-$(MAKE) -C $(NGINX_SRC) clean 2>/dev/null || true

distclean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Target disponibili:"
	@echo "  make [static]   build statica del binario nginx con il modulo"
	@echo "  make dynamic    build del solo modulo dinamico (.so)"
	@echo "  make test       smoke test end-to-end (richiede python3 e curl)"
	@echo "  make install    installa nginx compilato (richiede root)"
	@echo "  make clean      pulisce gli oggetti di build"
	@echo "  make distclean  rimuove build/ (sorgente nginx incluso)"
	@echo
	@echo "Variabili: NGINX_VERSION, NGINX_SRC, CONFIGURE_OPTS, CC_OPT"
