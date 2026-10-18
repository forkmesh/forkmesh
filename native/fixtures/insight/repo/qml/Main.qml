import QtQuick 2.15
import "components"

Item {
    width: 100
    function pick(index) {
        if (index > 2 && index < 9) {
            return index
        }
        return 0
    }
}
