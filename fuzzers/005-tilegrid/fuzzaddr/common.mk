N ?= 10
BUILD_DIR = build_$(XRAY_PART)
GENERATE_ARGS ?=
SPECIMENS := $(addprefix $(BUILD_DIR)/specimen_,$(shell seq -f '%03.0f' $(N)))
SPECIMENS_OK := $(addsuffix /OK,$(SPECIMENS))

database: $(BUILD_DIR)/segbits_tilegrid.tdb

$(BUILD_DIR)/segbits_tilegrid.tdb: $(SPECIMENS_OK)
	${XRAY_SEGMATCH} -o $(BUILD_DIR)/segbits_tilegrid.tdb $$(find $(BUILD_DIR) -name "segdata_tilegrid.txt")

# Retry a specimen up to 3 times: Vivado's FlexLM license manager can crash
# (SIGSEGV in libXil_lmgr11/freeaddrinfo) under concurrent license checkouts.
# Such crashes are transient and unrelated to the design, so retry before giving
# up. The success path is unchanged (first attempt short-circuits the chain).
$(SPECIMENS_OK):
	GENERATE_ARGS=${GENERATE_ARGS} bash ../fuzzaddr/generate.sh $(subst /OK,,$@) || \
	GENERATE_ARGS=${GENERATE_ARGS} bash ../fuzzaddr/generate.sh $(subst /OK,,$@) || \
	GENERATE_ARGS=${GENERATE_ARGS} bash ../fuzzaddr/generate.sh $(subst /OK,,$@)
	touch $@

clean:
	rm -rf build_*

clean_part:
	rm -rf $(BUILD_DIR)

.PHONY: database clean

