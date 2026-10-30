#include "WebEngine.h"

#include <QWidget>

#include <utility>

namespace forkmesh::web {

namespace {

Engine &registered()
{
    static Engine engine;
    return engine;
}

std::function<bool()> &modulePresent()
{
    static std::function<bool()> present;
    return present;
}

std::function<bool()> &moduleLoad()
{
    static std::function<bool()> load;
    return load;
}

bool ensureLoaded()
{
    if (loaded())
        return true;
    if (!built() || !moduleLoad())
        return false;
    moduleLoad()();
    return loaded();
}

}

View::~View() = default;

bool registerEngine(const Engine &engine)
{
    if (!engine.createProfile || !engine.createView)
        return false;
    registered() = engine;
    return true;
}

void setEngineModule(std::function<bool()> present, std::function<bool()> load)
{
    modulePresent() = std::move(present);
    moduleLoad() = std::move(load);
}

bool built()
{
#ifdef FORKMESH_WEB_ENGINE_BUILT
    return true;
#else
    return false;
#endif
}

bool loaded()
{
    return static_cast<bool>(registered().createView);
}

bool available()
{
    if (loaded())
        return true;
    return built() && modulePresent() && modulePresent()();
}

QObject *createProfile(const ProfileOptions &options, QObject *parent)
{
    if (!ensureLoaded())
        return nullptr;
    return registered().createProfile(options, parent);
}

View *createView(QObject *profile, const PageOptions &options, QWidget *parent)
{
    if (!profile || !ensureLoaded())
        return nullptr;
    return registered().createView(profile, options, parent);
}

View *viewOf(QWidget *widget)
{
    return dynamic_cast<View *>(widget);
}

}
