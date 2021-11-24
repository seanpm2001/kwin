/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick 2.12
import org.kde.kirigami 2.12 as Kirigami
import org.kde.kwin 3.0 as KWinComponents
import org.kde.kwin.private.effectkit 1.0
import org.kde.plasma.components 3.0 as PC3
import org.kde.plasma.core 2.0 as PlasmaCore

FocusScope {
    id: heap

    enum Direction {
        Left,
        Right,
        Up,
        Down
    }

    property alias model: windowsRepeater.model
    property alias layout: expoLayout.mode
    property int selectedIndex: -1
    property int animationDuration: PlasmaCore.Units.longDuration
    property bool animationEnabled: false
    property real padding: 0
    property var showOnly: []

    required property bool organized
    readonly property bool effectiveOrganized: expoLayout.ready && organized

    signal activated()

    ExpoLayout {
        id: expoLayout
        x: heap.padding
        y: heap.padding
        width: parent.width - 2 * heap.padding
        height: parent.height - 2 * heap.padding
        spacing: PlasmaCore.Units.largeSpacing

        Repeater {
            id: windowsRepeater

            Item {
                id: thumb

                required property QtObject client
                required property int index

                readonly property bool selected: heap.selectedIndex == index
                readonly property bool hidden: {
                    return heap.showOnly.length && heap.showOnly.indexOf(client.internalId) == -1;
                }

                state: {
                    if (heap.effectiveOrganized) {
                        return hidden ? "active-hidden" : "active";
                    }
                    return client.minimized ? "initial-minimized" : "initial";
                }

                visible: opacity > 0
                z: dragHandler.active ? 100 : client.stackingOrder

                KWinComponents.WindowThumbnailItem {
                    id: thumbSource
                    wId: thumb.client.internalId
                    state: dragHandler.active ? "drag" : "normal"

                    Drag.active: dragHandler.active
                    Drag.source: thumb.client
                    Drag.hotSpot: Qt.point(width * 0.5, height * 0.5)

                    states: [
                        State {
                            name: "normal"
                            PropertyChanges {
                                target: thumbSource
                                x: 0
                                y: 0
                                width: thumb.width
                                height: thumb.height
                            }
                        },
                        State {
                            name: "drag"
                            PropertyChanges {
                                target: thumbSource
                                x: -dragHandler.centroid.pressPosition.x * dragHandler.targetScale +
                                        dragHandler.centroid.position.x
                                y: -dragHandler.centroid.pressPosition.y * dragHandler.targetScale +
                                        dragHandler.centroid.position.y
                                width: cell.width * dragHandler.targetScale
                                height: cell.height * dragHandler.targetScale
                            }
                        }
                    ]

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.NoButton
                        cursorShape: dragHandler.active ? Qt.ClosedHandCursor : Qt.ArrowCursor
                    }
                }

                PlasmaCore.IconItem {
                    id: icon
                    width: PlasmaCore.Units.iconSizes.large
                    height: width
                    source: thumb.client.icon
                    anchors.horizontalCenter: thumbSource.horizontalCenter
                    anchors.bottom: thumbSource.bottom
                    anchors.bottomMargin: -height / 4
                    visible: !dragHandler.active
                }

                PC3.Label {
                    id: caption
                    width: Math.min(implicitWidth, thumbSource.width)
                    anchors.top: icon.bottom
                    anchors.horizontalCenter: icon.horizontalCenter
                    elide: Text.ElideRight
                    text: thumb.client.caption
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    visible: !dragHandler.active
                }

                ExpoCell {
                    id: cell
                    layout: expoLayout
                    enabled: !thumb.hidden
                    naturalX: thumb.client.x - targetScreen.geometry.x - expoLayout.Kirigami.ScenePosition.x
                    naturalY: thumb.client.y - targetScreen.geometry.y - expoLayout.Kirigami.ScenePosition.y
                    naturalWidth: thumb.client.width
                    naturalHeight: thumb.client.height
                    persistentKey: thumb.client.internalId
                    bottomMargin: icon.height / 4 + caption.height
                }

                states: [
                    State {
                        name: "initial"
                        PropertyChanges {
                            target: thumb
                            x: thumb.client.x - targetScreen.geometry.x - expoLayout.Kirigami.ScenePosition.x
                            y: thumb.client.y - targetScreen.geometry.y - expoLayout.Kirigami.ScenePosition.y
                            width: thumb.client.width
                            height: thumb.client.height
                        }
                    },
                    State {
                        name: "initial-minimized"
                        extend: "initial"
                        PropertyChanges {
                            target: thumb
                            opacity: 0
                        }
                    },
                    State {
                        name: "active"
                        PropertyChanges {
                            target: thumb
                            x: cell.x
                            y: cell.y
                            width: cell.width
                            height: cell.height
                        }
                    },
                    State {
                        name: "active-hidden"
                        extend: "active"
                        PropertyChanges {
                            target: thumb
                            opacity: 0
                        }
                    }
                ]

                component TweenBehavior : Behavior {
                    enabled: heap.animationEnabled && !dragHandler.active
                    NumberAnimation {
                        duration: heap.animationDuration
                        easing.type: Easing.InOutCubic
                    }
                }

                TweenBehavior on x {}
                TweenBehavior on y {}
                TweenBehavior on width {}
                TweenBehavior on height {}
                TweenBehavior on opacity {}

                PlasmaCore.FrameSvgItem {
                    anchors.fill: parent
                    anchors.margins: -PlasmaCore.Units.smallSpacing
                    imagePath: "widgets/viewitem"
                    prefix: "hover"
                    z: -1
                    visible: hoverHandler.hovered || selected
                }

                HoverHandler {
                    id: hoverHandler
                    onHoveredChanged: if (hovered != selected) {
                        heap.resetSelected();
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: {
                        KWinComponents.Workspace.activeClient = thumb.client;
                        heap.activated();
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.MiddleButton
                    onTapped: thumb.client.closeWindow()
                }

                DragHandler {
                    id: dragHandler
                    target: null

                    readonly property double targetScale: {
                        const localPressPosition = centroid.scenePressPosition.y - expoLayout.Kirigami.ScenePosition.y;
                        if (localPressPosition == 0) {
                            return 0.1
                        } else {
                            const localPosition = centroid.scenePosition.y - expoLayout.Kirigami.ScenePosition.y;
                            return Math.max(0.1, Math.min(localPosition / localPressPosition, 1))
                        }
                    }

                    onActiveChanged: {
                        if (!active) {
                            thumbSource.Drag.drop();
                        }
                    }
                }

                Loader {
                    LayoutMirroring.enabled: Qt.application.layoutDirection == Qt.RightToLeft
                    active: (hoverHandler.hovered || Kirigami.Settings.tabletMode || Kirigami.Settings.hasTransientTouchInput) && thumb.client.closeable && !dragHandler.active
                    anchors.right: thumbSource.right
                    anchors.rightMargin: PlasmaCore.Units.largeSpacing
                    anchors.top: thumbSource.top
                    anchors.topMargin: PlasmaCore.Units.largeSpacing
                    sourceComponent: PC3.Button {
                        icon.name: "window-close"
                        implicitWidth: PlasmaCore.Units.iconSizes.medium
                        implicitHeight: implicitWidth
                        onClicked: thumb.client.closeWindow();
                    }
                }

                Component.onDestruction: {
                    if (selected) {
                        heap.resetSelected();
                    }
                }
            }
        }
    }

    function findFirstItem() {
        for (let candidateIndex = 0; candidateIndex < windowsRepeater.count; ++candidateIndex) {
            const candidateItem = windowsRepeater.itemAt(candidateIndex);
            if (!candidateItem.hidden) {
                return candidateIndex;
            }
        }
        return -1;
    }

    function findNextItem(selectedIndex, direction) {
        if (selectedIndex == -1) {
            return findFirstItem();
        }

        const selectedItem = windowsRepeater.itemAt(selectedIndex);
        let nextIndex = -1;

        switch (direction) {
        case WindowHeap.Direction.Left:
            for (let candidateIndex = 0; candidateIndex < windowsRepeater.count; ++candidateIndex) {
                const candidateItem = windowsRepeater.itemAt(candidateIndex);
                if (candidateItem.hidden) {
                    continue;
                }

                if (candidateItem.y + candidateItem.height <= selectedItem.y) {
                    continue;
                } else if (selectedItem.y + selectedItem.height <= candidateItem.y) {
                    continue;
                }

                if (candidateItem.x + candidateItem.width < selectedItem.x + selectedItem.width) {
                    if (nextIndex == -1) {
                        nextIndex = candidateIndex;
                    } else {
                        const nextItem = windowsRepeater.itemAt(nextIndex);
                        if (candidateItem.x + candidateItem.width > nextItem.x + nextItem.width) {
                            nextIndex = candidateIndex;
                        }
                    }
                }
            }
            break;
        case WindowHeap.Direction.Right:
            for (let candidateIndex = 0; candidateIndex < windowsRepeater.count; ++candidateIndex) {
                const candidateItem = windowsRepeater.itemAt(candidateIndex);
                if (candidateItem.hidden) {
                    continue;
                }

                if (candidateItem.y + candidateItem.height <= selectedItem.y) {
                    continue;
                } else if (selectedItem.y + selectedItem.height <= candidateItem.y) {
                    continue;
                }

                if (selectedItem.x < candidateItem.x) {
                    if (nextIndex == -1) {
                        nextIndex = candidateIndex;
                    } else {
                        const nextItem = windowsRepeater.itemAt(nextIndex);
                        if (nextIndex == -1 || candidateItem.x < nextItem.x) {
                            nextIndex = candidateIndex;
                        }
                    }
                }
            }
            break;
        case WindowHeap.Direction.Up:
            for (let candidateIndex = 0; candidateIndex < windowsRepeater.count; ++candidateIndex) {
                const candidateItem = windowsRepeater.itemAt(candidateIndex);
                if (candidateItem.hidden) {
                    continue;
                }

                if (candidateItem.x + candidateItem.width <= selectedItem.x) {
                    continue;
                } else if (selectedItem.x + selectedItem.width <= candidateItem.x) {
                    continue;
                }

                if (candidateItem.y + candidateItem.height < selectedItem.y + selectedItem.height) {
                    if (nextIndex == -1) {
                        nextIndex = candidateIndex;
                    } else {
                        const nextItem = windowsRepeater.itemAt(nextIndex);
                        if (nextItem.y + nextItem.height < candidateItem.y + candidateItem.height) {
                            nextIndex = candidateIndex;
                        }
                    }
                }
            }
            break;
        case WindowHeap.Direction.Down:
            for (let candidateIndex = 0; candidateIndex < windowsRepeater.count; ++candidateIndex) {
                const candidateItem = windowsRepeater.itemAt(candidateIndex);
                if (candidateItem.hidden) {
                    continue;
                }

                if (candidateItem.x + candidateItem.width <= selectedItem.x) {
                    continue;
                } else if (selectedItem.x + selectedItem.width <= candidateItem.x) {
                    continue;
                }

                if (selectedItem.y < candidateItem.y) {
                    if (nextIndex == -1) {
                        nextIndex = candidateIndex;
                    } else {
                        const nextItem = windowsRepeater.itemAt(nextIndex);
                        if (candidateItem.y < nextItem.y) {
                            nextIndex = candidateIndex;
                        }
                    }
                }
            }
            break;
        }

        return nextIndex;
    }

    function resetSelected() {
        heap.selectedIndex = -1;
    }

    function selectNextItem(direction) {
        const nextIndex = findNextItem(heap.selectedIndex, direction);
        if (nextIndex != -1) {
            heap.selectedIndex = nextIndex;
        }
    }

    function selectLastItem(direction) {
        let last = heap.selectedIndex;
        while (true) {
            const next = findNextItem(last, direction);
            if (next == -1) {
                break;
            } else {
                last = next;
            }
        }
        if (last != -1) {
            heap.selectedIndex = last;
        }
    }

    onActiveFocusChanged: resetSelected();

    Keys.onPressed: {
        switch (event.key) {
        case Qt.Key_Up:
            selectNextItem(WindowHeap.Direction.Up);
            break;
        case Qt.Key_Down:
            selectNextItem(WindowHeap.Direction.Down);
            break;
        case Qt.Key_Left:
            selectNextItem(WindowHeap.Direction.Left);
            break;
        case Qt.Key_Right:
            selectNextItem(WindowHeap.Direction.Right);
            break;
        case Qt.Key_Home:
            selectLastItem(WindowHeap.Direction.Left);
            break;
        case Qt.Key_End:
            selectLastItem(WindowHeap.Direction.Right);
            break;
        case Qt.Key_PageUp:
            selectLastItem(WindowHeap.Direction.Up);
            break;
        case Qt.Key_PageDown:
            selectLastItem(WindowHeap.Direction.Down);
            break;
        case Qt.Key_Return:
        case Qt.Key_Space:
            let selectedItem = null;
            if (heap.selectedIndex != -1) {
                selectedItem = windowsRepeater.itemAt(heap.selectedIndex);
            } else {
                // If the window heap has only one visible window, activate it.
                for (let i = 0; i < windowsRepeater.count; ++i) {
                    const candidateItem = windowsRepeater.itemAt(i);
                    if (candidateItem.hidden) {
                        continue;
                    } else if (selectedItem) {
                        selectedItem = null;
                        break;
                    }
                    selectedItem = candidateItem;
                }
            }
            if (selectedItem) {
                KWinComponents.Workspace.activeClient = selectedItem.client;
                heap.activated();
            }
            break;
        default:
            return;
        }
        event.accepted = true;
    }
}
