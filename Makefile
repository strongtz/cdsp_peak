QAIC ?= ../QAIC/src/build/qaic/qaic
CLANG ?= clang
HEXAGON_TOOLS_ROOT ?= ../Hexagon_open_access.Core.19.0.02.Linux-ARM64
HEXAGON_CC ?= $(HEXAGON_TOOLS_ROOT)/Tools/bin/hexagon-clang

FASTRPC_INC ?= /usr/include/fastrpc
QAIC_INC ?= ../QAIC/inc
HEXAGON_INC ?= $(HEXAGON_TOOLS_ROOT)/Tools/target/hexagon/include

GEN_DIR := build/gen
HOST_DIR := build/host
ANDROID_DIR := build/android
ANDROID_SYSROOT := $(ANDROID_DIR)/sysroot
ANDROID_SYSROOT_LIB := $(ANDROID_SYSROOT)/system/lib64
ANDROID_VENDOR_LIB := $(ANDROID_SYSROOT)/vendor/lib64
DSP_RUN_DIR := build/dsp
DSP_ARCHES := v68 v73
DSP_DIR_v68 := build/dsp/v68
DSP_DIR_v73 := build/dsp/v73
INSTALL_DIR ?= $(abspath install)
ANDROID_INSTALL_DIR ?= $(abspath $(ANDROID_DIR)/install)

IDL := idl/cdsp_peak.idl
GEN_STAMP := $(GEN_DIR)/.qaic.stamp
GEN_OUTPUTS := $(GEN_DIR)/cdsp_peak.h $(GEN_DIR)/cdsp_peak_stub.c \
	$(GEN_DIR)/cdsp_peak_skel.c
GEN_SKEL_C_v68 := $(GEN_DIR)/cdsp_peak_skel_v68.c
GEN_SKEL_C_v73 := $(GEN_DIR)/cdsp_peak_skel_v73.c

HOST_STUB_SO := $(HOST_DIR)/libcdsp_peak_stub.so
HOST_TEST := $(HOST_DIR)/cdsp_peak
ANDROID_STUB_SO := $(ANDROID_DIR)/libcdsp_peak_stub.so
ANDROID_TEST := $(ANDROID_DIR)/cdsp_peak
ANDROID_SYSROOT_STAMP := $(ANDROID_SYSROOT)/.stamp
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
ADB ?= adb
ANDROID_API ?= 29
ANDROID_TARGET ?= aarch64-linux-android$(ANDROID_API)
ANDROID_FASTRPC_NAME ?= libcdsprpc.so
ANDROID_FASTRPC_LIB ?=
ANDROID_BUNDLE_FASTRPC ?= 0
ANDROID_CDSPRPC_SO := $(ANDROID_VENDOR_LIB)/$(ANDROID_FASTRPC_NAME)
ANDROID_PUSH_DIR ?= /data/local/tmp/cdsp_peak
ANDROID_UNSIGNED_PD ?= optional
ANDROID_UNSIGNED_PD_ARGS := $(if $(ANDROID_UNSIGNED_PD),--unsigned-pd $(ANDROID_UNSIGNED_PD),)

HOST_CFLAGS := -O2 -fPIC -Wall -Wextra --target=aarch64-linux-gnu \
	-I$(GEN_DIR) -I$(FASTRPC_INC) -I/usr/include/aarch64-linux-gnu \
	-mno-outline-atomics -pthread
HOST_LDFLAGS := --target=aarch64-linux-gnu -pthread
ANDROID_CFLAGS := -O2 -fPIC -Wall -Wextra --target=$(ANDROID_TARGET) \
	-D__errno_location=__errno \
	-I$(GEN_DIR) -I$(FASTRPC_INC) -I/usr/include/aarch64-linux-gnu \
	-mno-outline-atomics -pthread
ANDROID_LDFLAGS := --target=$(ANDROID_TARGET) -fuse-ld=lld -nostdlib \
	-L$(ANDROID_DIR) -L$(ANDROID_SYSROOT_LIB) -L$(ANDROID_VENDOR_LIB) \
	-Wl,--no-as-needed \
	-Wl,--unresolved-symbols=ignore-all \
	-Wl,-rpath,'$$ORIGIN' \
	-Wl,-rpath-link,$(ANDROID_SYSROOT_LIB) \
	-Wl,-rpath-link,$(ANDROID_VENDOR_LIB)
ANDROID_LDLIBS := -l:$(ANDROID_FASTRPC_NAME) -llog -lm -ldl -lc

DSP_CFLAGS_COMMON := -O3 -G0 -fPIC -Wall -Werror \
	-I$(GEN_DIR) -I$(FASTRPC_INC) -I$(QAIC_INC) -I$(HEXAGON_INC)
DSP_CFLAGS_v68 := $(DSP_CFLAGS_COMMON) -DCDSP_PEAK_ARCH=68 \
	-mv68 -mcpu=hexagonv68 \
	-mhvx=v68 -mhvx-ieee-fp
DSP_CFLAGS_v73 := $(DSP_CFLAGS_COMMON) -DCDSP_PEAK_ARCH=73 \
	-mv73 -mcpu=hexagonv73 \
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

.PHONY: all qaic run install android android-prepare-sysroot \
	android-fetch-sysroot android-install android-push android-run clean inspect FORCE

all: $(HOST_TEST) $(DSP_RUN_SKEL_SOS)

android: $(ANDROID_TEST) $(DSP_RUN_SKEL_SOS)

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

$(HOST_DIR) $(ANDROID_DIR) $(DSP_RUN_DIR) $(DSP_DIR_v68) $(DSP_DIR_v73):
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

android-prepare-sysroot:
	rm -f $(ANDROID_SYSROOT_STAMP)
	scripts/prepare_android_sysroot.sh $(ANDROID_SYSROOT) $(CLANG) \
		$(ANDROID_TARGET) $(ANDROID_FASTRPC_NAME) $(ANDROID_FASTRPC_LIB)

android-fetch-sysroot:
	rm -f $(ANDROID_SYSROOT_STAMP)
	scripts/fetch_android_sysroot.sh $(ANDROID_SYSROOT)

$(ANDROID_SYSROOT_STAMP): FORCE
	scripts/prepare_android_sysroot.sh $(ANDROID_SYSROOT) $(CLANG) \
		$(ANDROID_TARGET) $(ANDROID_FASTRPC_NAME) $(ANDROID_FASTRPC_LIB)

FORCE:

$(ANDROID_DIR)/android_crt.o: host/android_crt.c | $(ANDROID_DIR)
	$(CLANG) $(ANDROID_CFLAGS) -fno-stack-protector -c host/android_crt.c -o $@

$(ANDROID_DIR)/cdsp_peak_stub.o: $(GEN_STAMP) $(GEN_DIR)/cdsp_peak_stub.c | $(ANDROID_DIR)
	$(CLANG) $(ANDROID_CFLAGS) -c $(GEN_DIR)/cdsp_peak_stub.c -o $@

$(ANDROID_STUB_SO): $(ANDROID_DIR)/cdsp_peak_stub.o $(ANDROID_SYSROOT_STAMP)
	$(CLANG) $(ANDROID_LDFLAGS) -shared \
		-Wl,-soname,libcdsp_peak_stub.so \
		-o $@ $(ANDROID_DIR)/cdsp_peak_stub.o $(ANDROID_LDLIBS)

$(ANDROID_DIR)/cdsp_peak.o: host/cdsp_peak.c $(GEN_STAMP) | $(ANDROID_DIR)
	$(CLANG) $(ANDROID_CFLAGS) -c host/cdsp_peak.c -o $@

$(ANDROID_TEST): $(ANDROID_DIR)/android_crt.o $(ANDROID_DIR)/cdsp_peak.o $(ANDROID_STUB_SO) $(ANDROID_SYSROOT_STAMP)
	$(CLANG) $(ANDROID_LDFLAGS) \
		-Wl,-dynamic-linker,/system/bin/linker64 \
		-Wl,-e,_start \
		-o $@ $(ANDROID_DIR)/android_crt.o $(ANDROID_DIR)/cdsp_peak.o \
		-lcdsp_peak_stub $(ANDROID_LDLIBS)

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

android-install: android
	install -d $(ANDROID_INSTALL_DIR)
	rm -f $(ANDROID_INSTALL_DIR)/libcdsprpc*.so
	install -m 0755 $(ANDROID_TEST) $(ANDROID_INSTALL_DIR)/cdsp_peak
	install -m 0755 $(ANDROID_STUB_SO) $(ANDROID_INSTALL_DIR)/libcdsp_peak_stub.so
	install -m 0755 $(DSP_RUN_SKEL_SOS) $(ANDROID_INSTALL_DIR)/
	@if [ "$(ANDROID_BUNDLE_FASTRPC)" = "1" ]; then \
		if [ -z "$(ANDROID_FASTRPC_LIB)" ]; then \
			echo "ANDROID_BUNDLE_FASTRPC=1 requires ANDROID_FASTRPC_LIB=/path/to/real/device/$(ANDROID_FASTRPC_NAME)" >&2; \
			exit 2; \
		fi; \
		install -m 0755 $(ANDROID_CDSPRPC_SO) $(ANDROID_INSTALL_DIR)/$(ANDROID_FASTRPC_NAME); \
	fi

android-push: android-install
	$(ADB) shell 'rm -rf $(ANDROID_PUSH_DIR) && mkdir -p $(ANDROID_PUSH_DIR)'
	$(ADB) push $(ANDROID_INSTALL_DIR)/. $(ANDROID_PUSH_DIR)/
	$(ADB) shell 'chmod 755 $(ANDROID_PUSH_DIR)/cdsp_peak'

android-run: android-push
	$(ADB) shell 'cd $(ANDROID_PUSH_DIR) && LD_LIBRARY_PATH=. ADSP_LIBRARY_PATH=. DSP_LIBRARY_PATH=. ./cdsp_peak $(ANDROID_UNSIGNED_PD_ARGS) --scenario $(SCENARIO) $(THREAD_ARGS) $(RUN_ARGS)'

INSPECT_ARCH ?= v68
inspect: $(DSP_SKEL_SO_$(INSPECT_ARCH))
	$(HEXAGON_TOOLS_ROOT)/Tools/bin/hexagon-llvm-readelf -h -d $(DSP_SKEL_SO_$(INSPECT_ARCH))

clean:
	rm -rf build
