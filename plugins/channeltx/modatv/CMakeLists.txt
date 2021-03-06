project(modatv)

set(modatv_SOURCES
    atvmod.cpp
    atvmodbaseband.cpp
    atvmodreport.cpp
    atvmodsource.cpp
	atvmodplugin.cpp
    atvmodsettings.cpp
    atvmodwebapiadapter.cpp
)

set(modatv_HEADERS
	atvmod.h
    atvmodbaseband.h
    atvmodreport.h
    atvmodsource.h
	atvmodplugin.h
    atvmodsettings.h
    atvmodwebapiadapter.h
)

include_directories(
	${OpenCV_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/swagger/sdrangel/code/qt5/client
)

if(NOT SERVER_MODE)
    set(modatv_SOURCES
        ${modatv_SOURCES}
        atvmodgui.cpp

        atvmodgui.ui
    )
    set(modatv_HEADERS
        ${modatv_HEADERS}
        atvmodgui.h
    )

    set(TARGET_NAME modatv)
    set(TARGET_LIB "Qt5::Widgets")
    set(TARGET_LIB_GUI "sdrgui")
    set(INSTALL_FOLDER ${INSTALL_PLUGINS_DIR})
else()
    set(TARGET_NAME modatvsrv)
    set(TARGET_LIB "")
    set(TARGET_LIB_GUI "")
    set(INSTALL_FOLDER ${INSTALL_PLUGINSSRV_DIR})
endif()

add_library(${TARGET_NAME} SHARED
	${modatv_SOURCES}
)

target_link_libraries(${TARGET_NAME}
        Qt5::Core
        ${TARGET_LIB}
	sdrbase
	${TARGET_LIB_GUI}
        swagger
        "${OpenCV_LIBS}"
)

install(TARGETS ${TARGET_NAME} DESTINATION ${INSTALL_FOLDER})
