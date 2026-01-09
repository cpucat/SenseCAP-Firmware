set(TOOL_DIR ./tools)
file(GLOB_RECURSE TOOL_SRCS ${TOOL_DIR}/*.c)

set(IMG_DIR ./images)
file(GLOB_RECURSE IMG_SRCS ${IMG_DIR}/*.c)

set(UTIL_DIR ./util)
file(GLOB_RECURSE UTIL_SRCS ${UTIL_DIR}/*.c)

set(APP_DIR ./app)
file(GLOB_RECURSE APP_SRCS ${APP_DIR}/*.c)

idf_component_register(
      SRCS
        "main.c"
        ${TOOL_SRCS}
        ${IMG_SRCS}
        ${APP_SRCS}
        ${UTIL_SRCS}
      INCLUDE_DIRS
        "."
        ${TOOL_DIR}
        ${IMG_DIR}
        ${APP_DIR}
        ${UTIL_DIR}
    )

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
