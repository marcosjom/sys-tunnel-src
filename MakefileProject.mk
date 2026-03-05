
#-------------------------
# PROJECT
#-------------------------

$(eval $(call nbCall,nbInitProject))

NB_PROJECT_NAME             := tunnel

NB_PROJECT_CFLAGS           += -fPIC

NB_PROJECT_CXXFLAGS         += -fPIC -std=c++11

NB_PROJECT_INCLUDES         += \
   include \
   ../../sys-nbframework/sys-nbframework-src/include




#-------------------------
# TARGET
#-------------------------

$(eval $(call nbCall,nbInitTarget))

NB_TARGET_NAME              := tunnel-core

NB_TARGET_PREFIX            := lib

NB_TARGET_SUFIX             := .a

NB_TARGET_TYPE              := static

NB_TARGET_DEPENDS           += nbframework

NB_TARGET_FLAGS_ENABLES     += NB_LIB_TUNNEL

#-------------------------
# CODE GRP
#-------------------------

$(eval $(call nbCall,nbInitCodeGrp))

NB_CODE_GRP_NAME            := core

NB_CODE_GRP_FLAGS_REQUIRED  += NB_LIB_TUNNEL

NB_CODE_GRP_FLAGS_FORBIDDEN +=

NB_CODE_GRP_FLAGS_ENABLES   += NB_LIB_SSL

NB_CODE_GRP_SRCS            += \
    src/core/TNBuffs.c \
    src/core/TNCore.c \
    src/core/TNCoreCfg.c \
    src/core/TNCorePort.c \
    src/core/TNLyrBase64.c \
    src/core/TNLyrDump.c \
    src/core/TNLyrIO.c \
    src/core/TNLyrMask.c \
    src/core/TNLyrSsl.c

$(eval $(call nbCall,nbBuildCodeGrpRules))

#-------------------------
# TARGET RULES
#-------------------------

$(eval $(call nbCall,nbBuildTargetRules))








#-------------------------
# TARGET
#-------------------------

#Specific OS files
ifeq (,$(findstring Android,$(NB_CFG_HOST)))

$(eval $(call nbCall,nbInitTarget))

NB_TARGET_NAME              := tunnel-server

NB_TARGET_PREFIX            :=

NB_TARGET_SUFIX             :=

NB_TARGET_TYPE              := exe

NB_TARGET_DEPENDS           += tunnel-core

NB_TARGET_FLAGS_ENABLES     += NB_LIB_TUNNEL

#Specific OS
ifneq (,$(findstring Android,$(NB_CFG_HOST)))
  #Android
else
ifeq ($(OS),Windows_NT)
  #Windows
else
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S),Linux)
    #Linux
  endif
  ifeq ($(UNAME_S),Darwin)
    #OSX
    NB_TARGET_FRAMEWORKS    += Foundation Security
  endif
endif
endif


#-------------------------
# CODE GRP
#-------------------------

$(eval $(call nbCall,nbInitCodeGrp))

NB_CODE_GRP_NAME            := all

NB_CODE_GRP_FLAGS_REQUIRED  +=

NB_CODE_GRP_FLAGS_FORBIDDEN +=

NB_CODE_GRP_FLAGS_ENABLES   += NB_LIB_TUNNEL

NB_CODE_GRP_SRCS            += \
    src/main.c

$(eval $(call nbCall,nbBuildCodeGrpRules))

#-------------------------
# TARGET RULES
#-------------------------

$(eval $(call nbCall,nbBuildTargetRules))

endif









#-------------------------
# PROJECT RULES
#-------------------------

$(eval $(call nbCall,nbBuildProjectRules))
