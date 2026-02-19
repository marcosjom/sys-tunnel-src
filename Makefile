
LOCAL_PATH := $(call my-dir)

#Configure
NB_CFG_PRINT_INTERNALS := 1
NB_CFG_PRINT_INFO      := 0

#Import functions
include ../../../CltNicaraguaBinary/sys-nbframework/lib-nbframework-src/MakefileFuncs.mk

#Init workspace
$(eval $(call nbCall,nbInitWorkspace))

#Import projects
include ../../../CltNicaraguaBinary/sys-nbframework/lib-nbframework-src/MakefileProject.mk

#Specific OS
ifneq (,$(findstring Android,$(NB_CFG_HOST)))
  #Android
  include ../../../CltNicaraguaBinary/sys-auframework/lib-auframework-src/MakefileProject.mk
endif

#Specific OS
ifneq (,$(findstring Android,$(NB_CFG_HOST)))
  #Android
  include ../../../CltNicaraguaBinary/sys-auframework/lib-auframework-media-src/MakefileProject.mk
endif

#Specific OS
ifneq (,$(findstring Android,$(NB_CFG_HOST)))
  #Android
  include ../../../CltNicaraguaBinary/sys-auframework/lib-auframework-app-src/MakefileProject.mk
endif

#Project
include MakefileProject.mk

#Build workspace
$(eval $(call nbCall,nbBuildWorkspaceRules))

#Clean rule
clean:
	rm -r bin
	rm -r tmp

