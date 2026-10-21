# SPDX-License-Identifier: GPL-2.0-only

# Makefile for the kernel's cyclic redundancy check (CRC) library code

obj-$(CONFIG_CRC4) += crc4.o
obj-$(CONFIG_CRC7) += crc7.o
obj-$(CONFIG_CRC8) += crc8.o

#
# Ferret: CRC-16 has a Rust implementation. The C object is not built at all
# when the Rust variant is selected, so the symbol has exactly one definition.
#
ifeq ($(CONFIG_CRC16_RUST),y)
obj-y += crc16_rust.o
# ksymtab entry only; preserves EXPORT_SYMBOL(crc16) visibility. No algorithm.
obj-y += crc16_export.o
else
obj-$(CONFIG_CRC16) += crc16.o
endif

#
# Ferret: differential test. Compiles the unmodified C reference a second time
# under the test-only symbol crc16_c_ref (see docs/ferret/DECISIONS.md D-005).
#
ifeq ($(CONFIG_CRC16_DIFF_KUNIT_TEST),y)
CFLAGS_crc16_ref.o := -Dcrc16=crc16_c_ref
obj-y += crc16_ref.o crc16_diff_kunit.o

$(obj)/crc16_ref.o: $(src)/crc16.c FORCE
	$(call if_changed_rule,cc_o_c)
endif

#
# Ferret: CRC-CCITT has a Rust implementation. The C object is not built when
# the Rust variant is selected. crc_ccitt_byte() stays inline in the header and
# indexes the exported crc_ccitt_table[], so the table is part of the ABI.
#
ifeq ($(CONFIG_CRC_CCITT_RUST),y)
obj-y += crc_ccitt_rust.o
# ksymtab entries only (function + data); no algorithm.
obj-y += crc_ccitt_export.o
else
obj-$(CONFIG_CRC_CCITT) += crc-ccitt.o
endif

#
# Ferret: differential test for CRC-CCITT. The original C file is compiled a
# second time with both the function and its table renamed, so the test
# compares two genuinely independent implementations.
#
ifeq ($(CONFIG_CRC_CCITT_DIFF_KUNIT_TEST),y)
CFLAGS_crc_ccitt_ref.o := -Dcrc_ccitt=crc_ccitt_c_ref \
			 -Dcrc_ccitt_table=crc_ccitt_table_c_ref
obj-y += crc_ccitt_ref.o crc_ccitt_diff_kunit.o

$(obj)/crc_ccitt_ref.o: $(src)/crc-ccitt.c FORCE
	$(call if_changed_rule,cc_o_c)
endif

obj-$(CONFIG_CRC_ITU_T) += crc-itu-t.o

obj-$(CONFIG_CRC_T10DIF) += crc-t10dif.o
crc-t10dif-y := crc-t10dif-main.o
ifeq ($(CONFIG_CRC_T10DIF_ARCH),y)
CFLAGS_crc-t10dif-main.o += -I$(src)/$(SRCARCH)
crc-t10dif-$(CONFIG_ARM) += arm/crc-t10dif-core.o
crc-t10dif-$(CONFIG_ARM64) += arm64/crc-t10dif-core.o
crc-t10dif-$(CONFIG_PPC) += powerpc/crct10dif-vpmsum_asm.o
crc-t10dif-$(CONFIG_RISCV) += riscv/crc16_msb.o
crc-t10dif-$(CONFIG_X86) += x86/crc16-msb-pclmul.o
endif

obj-$(CONFIG_CRC32) += crc32.o
crc32-y := crc32-main.o
ifeq ($(CONFIG_CRC32_ARCH),y)
CFLAGS_crc32-main.o += -I$(src)/$(SRCARCH)
crc32-$(CONFIG_ARM) += arm/crc32-core.o
crc32-$(CONFIG_ARM64) += arm64/crc32-core.o
crc32-$(CONFIG_PPC) += powerpc/crc32c-vpmsum_asm.o
crc32-$(CONFIG_RISCV) += riscv/crc32_lsb.o riscv/crc32_msb.o
crc32-$(CONFIG_S390) += s390/crc32le-vx.o s390/crc32be-vx.o
crc32-$(CONFIG_SPARC) += sparc/crc32c_asm.o
crc32-$(CONFIG_X86) += x86/crc32-pclmul.o
crc32-$(CONFIG_X86_64) += x86/crc32c-3way.o
endif

obj-$(CONFIG_CRC64) += crc64.o
crc64-y := crc64-main.o
ifeq ($(CONFIG_CRC64_ARCH),y)
CFLAGS_crc64-main.o += -I$(src)/$(SRCARCH)
crc64-$(CONFIG_RISCV) += riscv/crc64_lsb.o riscv/crc64_msb.o
crc64-$(CONFIG_X86) += x86/crc64-pclmul.o
endif

obj-y += tests/

hostprogs := gen_crc32table gen_crc64table
clean-files := crc32table.h crc64table.h

$(obj)/crc32-main.o: $(obj)/crc32table.h
$(obj)/crc64-main.o: $(obj)/crc64table.h

quiet_cmd_crc32 = GEN     $@
      cmd_crc32 = $< > $@

quiet_cmd_crc64 = GEN     $@
      cmd_crc64 = $< > $@

$(obj)/crc32table.h: $(obj)/gen_crc32table
	$(call cmd,crc32)

$(obj)/crc64table.h: $(obj)/gen_crc64table
	$(call cmd,crc64)
