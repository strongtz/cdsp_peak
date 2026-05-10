QAIC ?= ../QAIC/src/build/qaic/qaic
CLANG ?= clang
HEXAGON_TOOLS_ROOT ?= ../Hexagon_open_access.Core.19.0.02.Linux-ARM64
HEXAGON_CC ?= $(HEXAGON_TOOLS_ROOT)/Tools/bin/hexagon-clang
HEXAGON_SDK_ROOT ?= ../Hexagon_SDK/6.5.0.0

FASTRPC_INC ?= /usr/include/fastrpc
QAIC_INC ?= ../QAIC/inc
HEXAGON_INC ?= $(HEXAGON_TOOLS_ROOT)/Tools/target/hexagon/include
HEXAGON_SDK_INC ?= $(HEXAGON_SDK_ROOT)/incs

GEN_DIR := build/gen
HOST_DIR := build/host
DSP_DIR := build/dsp/v68

IDL := idl/cdsp_peak.idl
GEN_STAMP := $(GEN_DIR)/.qaic.stamp

HOST_STUB_SO := $(HOST_DIR)/libcdsp_peak_stub.so
HOST_TEST := $(HOST_DIR)/cdsp_peak
DSP_SKEL_SO := $(DSP_DIR)/libcdsp_peak_skel.so

SCENARIO ?= all
THREADS ?=
RUN_ARGS ?=
THREAD_ARGS := $(if $(THREADS),--threads $(THREADS),)

HOST_CFLAGS := -O2 -fPIC -Wall -Wextra --target=aarch64-linux-gnu \
	-I$(GEN_DIR) -I$(FASTRPC_INC) -I/usr/include/aarch64-linux-gnu \
	-mno-outline-atomics -pthread
HOST_LDFLAGS := --target=aarch64-linux-gnu -pthread

DSP_CFLAGS := -O3 -G0 -fPIC -Wall -Werror -mv68 -mcpu=hexagonv68 \
	-mhvx=v68 -mhvx-ieee-fp \
	-I$(GEN_DIR) -I$(FASTRPC_INC) -I$(QAIC_INC) -I$(HEXAGON_INC)
DSP_POWER_CFLAGS := -O3 -G0 -fPIC -Wall -Werror -mv68 -mcpu=hexagonv68 \
	-mhvx=v68 -mhvx-ieee-fp \
	-I$(GEN_DIR) -I$(HEXAGON_SDK_INC) -I$(HEXAGON_SDK_INC)/stddef \
	-I$(FASTRPC_INC) -I$(QAIC_INC) -I$(HEXAGON_INC)
DSP_HMX_CFLAGS := $(DSP_CFLAGS) -mhmx
DSP_LDFLAGS := -shared -G0 -mv68 -mcpu=hexagonv68 -mhvx=v68 -mhvx-ieee-fp \
	-Wl,-Bsymbolic \
	-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free \
	-Wl,--wrap=realloc -Wl,--wrap=memalign \
	-Wl,-soname,libcdsp_peak_skel.so

.PHONY: all qaic run clean inspect

all: $(HOST_TEST) $(DSP_SKEL_SO)

qaic: $(GEN_STAMP)

$(GEN_STAMP): $(IDL) $(QAIC)
	mkdir -p $(GEN_DIR)
	$(QAIC) -m dll -st -I$(QAIC_INC) -I$(FASTRPC_INC) -o $(GEN_DIR) $(IDL)
	touch $@

$(HOST_DIR) $(DSP_DIR):
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

$(DSP_DIR)/cdsp_peak_skel.o: $(GEN_STAMP) $(GEN_DIR)/cdsp_peak_skel.c | $(DSP_DIR)
	$(HEXAGON_CC) $(DSP_CFLAGS) -c $(GEN_DIR)/cdsp_peak_skel.c -o $@

$(DSP_DIR)/cdsp_peak_imp.o: dsp/cdsp_peak_imp.c $(GEN_STAMP) | $(DSP_DIR)
	$(HEXAGON_CC) $(DSP_CFLAGS) -c dsp/cdsp_peak_imp.c -o $@

$(DSP_DIR)/cdsp_peak_power.o: dsp/cdsp_peak_power.c $(GEN_STAMP) | $(DSP_DIR)
	$(HEXAGON_CC) $(DSP_POWER_CFLAGS) -c dsp/cdsp_peak_power.c -o $@

$(DSP_DIR)/cdsp_peak_hmx.o: dsp/cdsp_peak_hmx.c $(GEN_STAMP) | $(DSP_DIR)
	$(HEXAGON_CC) $(DSP_HMX_CFLAGS) -c dsp/cdsp_peak_hmx.c -o $@

$(DSP_SKEL_SO): $(DSP_DIR)/cdsp_peak_skel.o $(DSP_DIR)/cdsp_peak_imp.o $(DSP_DIR)/cdsp_peak_power.o $(DSP_DIR)/cdsp_peak_hmx.o
	$(HEXAGON_CC) $(DSP_LDFLAGS) -o $@ $^

run: all
	LD_LIBRARY_PATH=$(abspath $(HOST_DIR)) \
	ADSP_LIBRARY_PATH=$(abspath $(DSP_DIR)) \
	DSP_LIBRARY_PATH=$(abspath $(DSP_DIR)) \
	$(HOST_TEST) --scenario $(SCENARIO) $(THREAD_ARGS) $(RUN_ARGS)

inspect: $(DSP_SKEL_SO)
	$(HEXAGON_TOOLS_ROOT)/Tools/bin/hexagon-llvm-readelf -h -d $(DSP_SKEL_SO)

clean:
	rm -rf build
