import QtQuick 2.0
import QtQuick.Controls 1.0
import MpvPlayer 1.0

Item {
    width: 1280
    height: 720

    GridView {
        id: grid

        anchors.fill: parent
        focus: true

        property int columns: Math.sqrt(count) > 1 ? Math.ceil(Math.sqrt(count)) : 1
        cellWidth: width / columns
        cellHeight: height / Math.ceil(count / columns)

        model: players
        delegate: MpvPlayerQuickObject {
            id: player
            width: grid.cellWidth
            height: grid.cellHeight

            url: model.url
            paused: model.paused
        }
    }
}
