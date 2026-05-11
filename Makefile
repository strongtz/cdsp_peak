QAIC ?= ../QAIC/src/build/qaic/qaic
CLANG ?= clang
HEXAGON_TOOLS_ROOT ?= ../Hexagon_open_access.Core.19.0.02.Linux-ARM64
HEXAGON_CC ?= $(HEXAGON_TOOLS_ROOT)/Tools/bin/hexagon-clang

FASTRPC_INC ?= /usr/include/fastrpc
QAIC_INC ?= ../QAIC/inc
HEXAGON_INC ?= $(HEXAGON_TOOLS_ROOT)/Tools/target/hexagon/include

GEN_DIR := build/gen
HOST_DIR := build/host
DSP_RUN_DIR := build/dsp
DSP_ARCHES := v68 v73
DSP_DIR_v68 := build/dsp/v68
DSP_DIR_v73 := build/dsp/v73
INSTALL_DIR ?= $(abspath install)

IDL := idl/cdsp_peak.idl
GEN_STAMP := $(GEN_DIR)/.qaic.stamp
GEN_OUTPUTS := $(GEN_DIR)/cdsp_peak.h $(GEN_DIR)/cdsp_peak_stub.c \
	$(GEN_DIR)/cdsp_peak_skel.c
GEN_SKEL_C_v68 := $(GEN_DIR)/cdsp_peak_skel_v68.c
GEN_SKEL_C_v73 := $(GEN_DIR)/cdsp_peak_skel_v73.c

HOST_STUB_SO := $(HOST_DIR)/libcdsp_peak_stub.so
HOST_TEST := $(HOST_DIR)/cdsp_peak
DSP_SKEL_SO_v68 := $(DSP_DIR_v68)/libcdsp_peak_skel_v68.so
DSP_SKEL_SO_v73 := $(DSP_DIR_v73)/libcdsp_peak_skel_v73.so
DSP_SKEL_SOS := $(DSP_SKEL_SO_v68) $(DSP_SKEL_SO_v73)
DSP_RUN_SKEL_SO_v68 := $(DSP_RUN_DIR)/libcdsp_peak_skel_v68.so
DSP_RUN_SKEL_SO_v73 := $(DSP_RUN_DIR)/libcdsp_peak_skel_v73.so
DSP_RUN_SKEL_SOS := $(DSP_RUN_SKEL_SO_v68) $(DSP_RUN_SKEL_SO_v73)

SCENARIO ?= all
THREADS ?=
RUN_ARGS ?=
THREAD_ARGS := $(if $(THREADS),--threads $(THREADS),)

HOST_CFLAGS := -O2 -fPIC -Wall -Wextra --target=aarch64-linux-gnu \
	-I$(GEN_DIR) -I$(FASTRPC_INC) -I/usr/include/aarch64-linux-gnu \
	-mno-outline-atomics -pthread
HOST_LDFLAGS := --target=aarch64-linux-gnu -pthread

DSP_CFLAGS_COMMON := -O3 -G0 -fPIC -Wall -Werror \
	-I$(GEN_DIR) -I$(FASTRPC_INC) -I$(QAIC_INC) -I$(HEXAGON_INC)
DSP_CFLAGS_v68 := $(DSP_CFLAGS_COMMON) -mv68 -mcpu=hexagonv68 \
	-mhvx=v68 -mhvx-ieee-fp
DSP_CFLAGS_v73 := $(DSP_CFLAGS_COMMON) -mv73 -mcpu=hexagonv73 \
	-mhvx=v73 -mhvx-ieee-fp
DSP_HMX_CFLAGS_v68 := $(DSP_CFLAGS_v68) -mhmx
DSP_HMX_CFLAGS_v73 := $(DSP_CFLAGS_v73) -mhmx
DSP_LDFLAGS_COMMON := -shared -G0 \
	-Wl,-Bsymbolic \
	-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free \
	-Wl,--wrap=realloc -Wl,--wrap=memalign
DSP_LDFLAGS_v68 := -mv68 -mcpu=hexagonv68 -mhvx=v68 -mhvx-ieee-fp \
	$(DSP_LDFLAGS_COMMON) -Wl,-soname,libcdsp_peak_skel_v68.so
DSP_LDFLAGS_v73 := -mv73 -mcpu=hexagonv73 -mhvx=v73 -mhvx-ieee-fp \
	$(DSP_LDFLAGS_COMMON) -Wl,-soname,libcdsp_peak_skel_v73.so

.PHONY: all qaic run install clean inspect

all: $(HOST_TEST) $(DSP_RUN_SKEL_SOS)

qaic: $(GEN_STAMP)

$(GEN_STAMP): $(IDL) $(QAIC)
	mkdir -p $(GEN_DIR)
	$(QAIC) -m dll -st -I$(QAIC_INC) -I$(FASTRPC_INC) -o $(GEN_DIR) $(IDL)
	touch $@

$(GEN_OUTPUTS): $(GEN_STAMP)

$(GEN_DIR)/cdsp_peak_skel_%.c: $(GEN_STAMP) $(GEN_DIR)/cdsp_peak_skel.c
	sed -e 's#libcdsp_peak_skel\.so#libcdsp_peak_skel_$*.so#g' \
		-e 's#cdsp_peak_skel_handle_invoke_uri\[[0-9][0-9]*+1\]#cdsp_peak_skel_handle_invoke_uri[]#' \
		$(GEN_DIR)/cdsp_peak_skel.c > $@

$(HOST_DIR) $(DSP_RUN_DIR) $(DSP_DIR_v68) $(DSP_DIR_v73):
	mkdir -p $@

$(HOST_DIR)/cdsp_peak_stub.o: $(GEN_STAMP) $(GEN_DIR)/cdsp_peak_stub.c | $(HOST_DIR)
	$(CLANG) $(HOST_CFLAGS) -c $(GEN_DIR)/cdsp_peak_stub.c -o $@

$(HOST_STUB_SO): $(HOST_DIR)/cdsp_peak_stub.o
	$(CLANG) $(HOST_LDFLAGS) -shared -Wl,-soname,libcdsp_peak_stub.so -o $@ $^ -lcdsprpc

$(HOST_DIR)/cdsp_peak.o: host/cdsp_peak.c $(GEN_STAMP) | $(HOST_DIR)
	$(CLANG) $(HOST_CFLAGS) -c host/cdsp_peak.c -o $@

$(HOST_TEST): $(HOST_DIR)/cdsp_peak.o $(HOST_STUB_SO)
	$(CLANG) $(HOST_LDFLAGS) -o $@ $(HOST_DIR)/cdsp_peak.o \
		-L$(HOST_DIR) -lcdsp_peak_stub -lcdsprpc -lbsd -Wl,-rpath,'$$ORIGIN'

define DSP_ARCH_RULES
$$(DSP_DIR_$(1))/cdsp_peak_skel.o: $$(GEN_SKEL_C_$(1)) | $$(DSP_DIR_$(1))
	$$(HEXAGON_CC) $$(DSP_CFLAGS_$(1)) -c $$(GEN_SKEL_C_$(1)) -o $$@

$$(DSP_DIR_$(1))/cdsp_peak_imp.o: dsp/cdsp_peak_imp.c $$(GEN_STAMP) | $$(DSP_DIR_$(1))
	$$(HEXAGON_CC) $$(DSP_CFLAGS_$(1)) -c dsp/cdsp_peak_imp.c -o $$@

$$(DSP_DIR_$(1))/cdsp_peak_power.o: dsp/cdsp_peak_power.c $$(GEN_STAMP) | $$(DSP_DIR_$(1))
	$$(HEXAGON_CC) $$(DSP_CFLAGS_$(1)) -c dsp/cdsp_peak_power.c -o $$@

$$(DSP_DIR_$(1))/cdsp_peak_hmx.o: dsp/cdsp_peak_hmx.c $$(GEN_STAMP) | $$(DSP_DIR_$(1))
	$$(HEXAGON_CC) $$(DSP_HMX_CFLAGS_$(1)) -c dsp/cdsp_peak_hmx.c -o $$@

$$(DSP_SKEL_SO_$(1)): $$(DSP_DIR_$(1))/cdsp_peak_skel.o $$(DSP_DIR_$(1))/cdsp_peak_imp.o $$(DSP_DIR_$(1))/cdsp_peak_power.o $$(DSP_DIR_$(1))/cdsp_peak_hmx.o
	$$(HEXAGON_CC) $$(DSP_LDFLAGS_$(1)) -o $$@ $$^
endef

$(foreach arch,$(DSP_ARCHES),$(eval $(call DSP_ARCH_RULES,$(arch))))

define DSP_RUN_RULE
$$(DSP_RUN_SKEL_SO_$(1)): $$(DSP_SKEL_SO_$(1)) | $$(DSP_RUN_DIR)
	cp $$< $$@
endef

$(foreach arch,$(DSP_ARCHES),$(eval $(call DSP_RUN_RULE,$(arch))))

run: all
	LD_LIBRARY_PATH=$(abspath $(HOST_DIR)) \
	ADSP_LIBRARY_PATH=$(abspath $(DSP_RUN_DIR)) \
	DSP_LIBRARY_PATH=$(abspath $(DSP_RUN_DIR)) \
	$(HOST_TEST) --scenario $(SCENARIO) $(THREAD_ARGS) $(RUN_ARGS)

install: all
	install -d $(INSTALL_DIR)
	install -m 0755 $(HOST_TEST) $(INSTALL_DIR)/cdsp_peak
	install -m 0755 $(HOST_STUB_SO) $(INSTALL_DIR)/libcdsp_peak_stub.so
	install -m 0755 $(DSP_RUN_SKEL_SOS) $(INSTALL_DIR)/

INSPECT_ARCH ?= v68
inspect: $(DSP_SKEL_SO_$(INSPECT_ARCH))
	$(HEXAGON_TOOLS_ROOT)/Tools/bin/hexagon-llvm-readelf -h -d $(DSP_SKEL_SO_$(INSPECT_ARCH))

clean:
	rm -rf build
