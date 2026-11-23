# hakuX-Boosted: our multi-threaded test titles, built in xsdk's tree exactly as its probes are (its CFLAGS, its link
# with src/xsdk.ld and libxsdk.a, its elf2xex rule). The workflow copies boosted/xsdk-tests/ into xsdk's tests/ and
# this file beside xsdk's Makefile, then runs: make -f Makefile -f hakux.mk TOOLCHAIN=... hakux_titles
HAKUX_TITLES := hakux_waits hakux_order hakux_reserve

.PHONY: hakux_titles
hakux_titles: $(HAKUX_TITLES:%=$(BUILD)/%.xex)

$(BUILD)/hakux_%.elf: $(BUILD)/tests/hakux_%.o $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -o $@ $(BUILD)/tests/hakux_$*.o $(LIBS)
