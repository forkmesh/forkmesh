#include <QApplication>

#include <iostream>

int runAgentActivityModelTests();
int runAgentActivityViewTests();

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const int model = runAgentActivityModelTests();
    const int view = runAgentActivityViewTests();
    if (model != 0 || view != 0)
        std::cout << "agent-watch: model " << model << ", view " << view << "\n";
    return model == 0 && view == 0 ? 0 : 1;
}
