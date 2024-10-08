﻿ifeq ($(DEBUG),true)
CC = g++ -std=c++11 -g 
VERSION = debug
else
CC = g++ -std=c++11
VERSION = release
endif

SRCS = $(wildcard *.cxx)

OBJS = $(SRCS:.cxx=.o)

DEPS = $(SRCS:.cxx=.d)

BIN := $(addprefix $(BUILD_ROOT)/,$(BIN))

LINK_OBJ_DIR = $(BUILD_ROOT)/app/link_obj
DEP_DIR = $(BUILD_ROOT)/app/dep

$(shell mkdir -p $(LINK_OBJ_DIR))
$(shell mkdir -p $(DEP_DIR))

OBJS := $(addprefix $(LINK_OBJ_DIR)/,$(OBJS))
DEPS := $(addprefix $(DEP_DIR)/,$(DEPS))

LINK_OBJ = $(wildcard $(LINK_OBJ_DIR)/*.o)
LINK_OBJ += $(OBJS)

all:$(DEPS) $(OBJS) $(BIN)

ifneq ("$(wildcard $(DEPS))","")
include $(DEPS)  
endif

$(BIN):$(LINK_OBJ)
	@echo "------------------------build $(VERSION) mode--------------------------------!!!"

	$(CC) -o $@ $^ -lpthread

$(LINK_OBJ_DIR)/%.o:%.cxx
	$(CC) -I$(INCLUDE_PATH) -o $@ -c $(filter %.cxx,$^)

$(DEP_DIR)/%.d:%.cxx
	echo -n $(LINK_OBJ_DIR)/ > $@
	$(CC) -I$(INCLUDE_PATH) -MM $^ >> $@