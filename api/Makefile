
#
# Tencent is pleased to support the open source community by making MSEC available.
#
# Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.
#
# Licensed under the GNU General Public License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License. You may
# obtain a copy of the License at
#
#     https://opensource.org/licenses/GPL-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the
# License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
# either express or implied. See the License for the specific language governing permissions
# and limitations under the License.
#

CC=gcc

# CFLAGS= -pipe -fno-ident -fPIC -shared -z defs -O2 -Wall -D_GNU_SOURCE -Wno-write-strings -Werror
CFLAGS= -std=gnu99 -pipe -fno-ident -z defs -g -ggdb3 -Wall -D_GNU_SOURCE -Wno-write-strings -Werror
ifeq ($(ARCH),32)
        CFLAGS +=  -march=pentium4 -m32 -pthread
else
        # CFLAGS +=  -m64 -pthread
endif

INC= -I./ -I../comm
LIB= -L../comm -lcomm -L../api -lnlbapi ../third_party/zookeeper/lib/libzookeeper_mt.a -lm
TARGET= libnlbapi.a
OBJ= ../comm/hash.o ../comm/nlbfile.o ../comm/utils.o ../comm/comm.o ../comm/routeproto.o ../comm/nlbrand.o nlbapi.o

all: $(TARGET) nlbapi_test

$(TARGET): $(OBJ)
	@echo -e  Linking $(CYAN)$@$(RESET) ...$(RED)
	@ar crs $@ $^ $(CRESET)
	@chmod +x $@

nlbapi_test:nlbapi_test.o
	@$(CC) -o $@ $^ $(CFLAGS) $(LIB) $(CRESET)

include ../incl_comm.mk

clean:
	@rm -f $(OBJ) $(TARGET)
	rm -rf ./nlbapi_test.o ./nlbapi_test

cleanext:
	@rm -f $(OBJ)
	rm -rf ./nlbapi_test.o ./nlbapi_test
