target_sources(kwin PRIVATE libeisbackend.cpp connection.cpp device.cpp)
target_link_libraries(kwin Libeis::Libeis)
target_compile_definitions(kwin PRIVATE KWIN_BUILD_EIS)
ecm_qt_declare_logging_category(kwin
    HEADER libeis_logging.h
    IDENTIFIER KWIN_EIS
    CATEGORY_NAME kwin_libeis
    DEFAULT_SEVERITY Debug
)
