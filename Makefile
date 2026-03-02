
LOCAL_PATH := $(call my-dir)

#Configure
NB_CFG_PRINT_INTERNALS := 0
NB_CFG_PRINT_INFO      := 0

#Import functions
include ../../sys-nbframework/sys-nbframework-src/MakefileFuncs.mk

#Init workspace
$(eval $(call nbCall,nbInitWorkspace))

#Import projects
include ../../sys-nbframework/sys-nbframework-src/MakefileProject.mk

#Project
include MakefileProject.mk

#Build workspace
$(eval $(call nbCall,nbBuildWorkspaceRules))

#Clean rule
clean:
	rm -r bin
	rm -r tmp

