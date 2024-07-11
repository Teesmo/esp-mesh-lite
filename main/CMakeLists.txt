idf_component_register(SRCS "local_control.c"
			    "deep_sleep_example_main.c"
                       INCLUDE_DIRS "../include"
	       				"."
			)
	# Create a partition using CMake on Host System, Windows does not support littlefs partitioning
	#if(NOT_CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
littlefs_create_partition_image(storage ../meter_files FLASH_IN_PROJECT)
		#else()
		#fail_at_build_time(littlefs "Windows does not support LittleFS partition generation")
		#endif()
