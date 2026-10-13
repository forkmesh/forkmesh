#pragma once

#include <QList>
#include <QObject>
#include <QPointer>

#include <memory>
#include <utility>

namespace forkmesh {

class DeferredSocketRetirement
{
    struct State {
        int depth = 0;
        QList<QPointer<QObject>> pending;
    };

public:
    class Scope
    {
    public:
        explicit Scope(DeferredSocketRetirement &retirement)
            : m_state(retirement.m_state)
        {
            ++m_state->depth;
        }

        ~Scope()
        {
            if (--m_state->depth > 0)
                return;
            const QList<QPointer<QObject>> pending =
                std::exchange(m_state->pending, {});
            for (const QPointer<QObject> &object : pending) {
                if (object)
                    object->deleteLater();
            }
        }

        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        std::shared_ptr<State> m_state;
    };

    void retire(QObject *object)
    {
        if (!object)
            return;
        if (m_state->depth > 0) {
            m_state->pending.append(object);
            return;
        }
        object->deleteLater();
    }

    bool dispatching() const { return m_state->depth > 0; }

private:
    std::shared_ptr<State> m_state = std::make_shared<State>();
};

} // namespace forkmesh
