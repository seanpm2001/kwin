# SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>
#
# SPDX-License-Identifier: BSD-3-Clause

add_library(effectkit
    expoarea.cpp
    expolayout.cpp
    plugin.cpp
)

target_link_libraries(effectkit
    kwineffects

    Qt5::Core
    Qt5::Gui
    Qt5::Qml
    Qt5::Quick
)

install(DIRECTORY qml/ DESTINATION ${KDE_INSTALL_QMLDIR}/org/kde/kwin/private/effectkit)
install(TARGETS effectkit DESTINATION ${KDE_INSTALL_QMLDIR}/org/kde/kwin/private/effectkit)
install(FILES qmldir DESTINATION ${KDE_INSTALL_QMLDIR}/org/kde/kwin/private/effectkit)
