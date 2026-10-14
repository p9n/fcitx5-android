add_definitions(-DFCITX_GETTEXT_DOMAIN=\"fcitx5-android\")

add_library(boshiamy MODULE engine.cpp dictionary.cpp)
target_link_libraries(boshiamy Fcitx5::Core Fcitx5::Config Fcitx5::Utils)

configure_file(boshiamy.conf.in.in boshiamy.conf.in @ONLY)
fcitx5_translate_desktop_file(${CMAKE_CURRENT_BINARY_DIR}/boshiamy.conf.in boshiamy.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/boshiamy.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
        COMPONENT config)

# Radical tables converted from liu-uni*.tab by tools/tab2txt.py,
# and Zhuyin table (pinyi.txt) from UCL_LIU (MIT License).
install(FILES
        data/boshiamy-t.txt
        data/boshiamy-c.txt
        data/boshiamy-ct.txt
        data/boshiamy-j.txt
        data/pinyi.txt
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/boshiamy"
        COMPONENT config)
