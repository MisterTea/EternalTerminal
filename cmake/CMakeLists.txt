include(ExternalProject)

if(BUILD_TEST)
  ExternalProject_Add(
	googletest_external
	URL ${CMAKE_SOURCE_DIR}/cmake/external/googletest
	CONFIGURE_COMMAND "" 
	BUILD_COMMAND "" 
	INSTALL_COMMAND ""
	BUILD_IN_SOURCE 1 
	LOG_CONFIGURE 1 
	LOG_BUILD 1 
	)
#Add our own configure step
ExternalProject_Add_Step(
	googletest_external 
	googletest_configure	
	WORKING_DIRECTORY <SOURCE_DIR>/
	DEPENDEES patch
	DEPENDERS configure
  COMMAND cmake ./  
	ALWAYS 1
	LOG 1
	)
#Add our own build step
ExternalProject_Add_Step(
	googletest_external 
	googletest_build
	WORKING_DIRECTORY <SOURCE_DIR>/
	DEPENDEES configure
	DEPENDERS build
	COMMAND make 
	ALWAYS 1
	LOG 1
	)
  #
  # Specify include dir
  # PARENT_SCOPE points to root directory
  #
  ExternalProject_Get_Property(googletest_external source_dir)
  set(GTEST_INC_DIR ${source_dir}/googletest/include PARENT_SCOPE)
  set(GMOCK_INC_DIR ${source_dir}/googlemock/include PARENT_SCOPE)

  ExternalProject_Get_Property(googletest_external binary_dir)
  set(GTEST_LIB_DIR ${binary_dir}/googlemock/gtest PARENT_SCOPE)
  set(GMOCK_LIB_DIR ${binary_dir}/googlemock PARENT_SCOPE)
endif()

