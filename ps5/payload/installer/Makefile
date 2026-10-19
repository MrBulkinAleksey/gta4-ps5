# GTA IV for PS5 - the installer payload (GPL-3.0-or-later), packaged in the app as
# payloads/gta4-ps5-installer.elf (ps5/tools/ps5_package.sh builds it).
#   make PS5_PAYLOAD_SDK=<sdk> [ZLIB=<zlib built for the PS5: include/ and lib/libz.a>]
# ZLIB defaults to the one ps5-plume-triangle builds beside the SDK.
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF ?= gta4-ps5-installer.elf
ZLIB ?= $(PS5_PAYLOAD_SDK)/../zlib/root/usr
REXSDK := ../../../glue/rexglue-sdk-main
INSTALL := $(REXSDK)/gta4-recomp/src/install
CRYPTO := $(REXSDK)/thirdparty/crypto

SRC := installer.cpp store_disc.cpp store_stfs.cpp $(INSTALL)/gta4_rpf_extractor.cpp
HDR := store_disc.hpp store_stfs.hpp store_zip.hpp include/rex/platform.h $(INSTALL)/gta4_install_io.h \
       $(INSTALL)/gta4_rpf_extractor.h $(REXSDK)/include/rex/filesystem/direct_io.h
CXXFLAGS := -std=c++20 -O2 -Wall -Werror -Iinclude -I$(INSTALL) -I$(REXSDK)/include -I$(CRYPTO) -I$(ZLIB)/include

all: $(ELF)

rijndael.o: $(CRYPTO)/rijndael-alg-fst.c
	$(CC) -O2 -c -I$(CRYPTO) -o $@ $<

$(ELF): $(SRC) $(HDR) rijndael.o
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) rijndael.o $(ZLIB)/lib/libz.a -lpthread
	$(PS5_PAYLOAD_SDK)/bin/llvm-strip --strip-debug $@ 2>/dev/null || true

clean:
	rm -f $(ELF) rijndael.o
