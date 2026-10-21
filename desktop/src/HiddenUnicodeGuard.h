#pragma once

class QApplication;
class QWidget;

namespace forkmesh::hiddenunicode {

void installInputGuard(QApplication *app);
void guardWidget(QWidget *widget);

}
