QT += core

CONFIG += c++11

INCLUDEPATH += $$PWD/src

DEFINES += ELPP_QT_LOGGING \
           ELPP_STL_LOGGING \
           ELPP_STRICT_SIZE_CHECK ELPP_UNICODE \
           ELPP_MULTI_LOGGER_SUPPORT \
           ELPP_THREAD_SAFE \
           ELPP_UNICODE \
           ELPP_NO_DEFAULT_LOG_FILE

SOURCES += $$PWD/src/easylogging++.cc

HEADERS += $$PWD/src/easylogging++.h
