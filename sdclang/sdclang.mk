# SD-LLVM is disabled on darwin due missing binaries
ifneq ($(HOST_OS),darwin)

expected_sdclang_path := prebuilts/snapdragon-llvm/toolchains/llvm-Snapdragon_LLVM_for_Android_3.8/prebuilt/linux-x86_64/bin

ifneq ($(strip $(wildcard $(expected_sdclang_path))),)
    SDCLANG := true
    SDCLANG_PATH := $(expected_sdclang_path)
    SDCLANG_LTO_DEFS := device/qcom/common/sdclang/sdllvm-lto-defs.mk
endif

expected_sdclang_path :=

endif # HOST_OS != darwin
