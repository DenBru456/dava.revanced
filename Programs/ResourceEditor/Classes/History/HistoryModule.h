#pragma once

#include <TArc/Core/ClientModule.h>
#include <TArc/Core/FieldBinder.h>

#include <Reflection/Reflection.h>

#include <QPointer>

#include <memory>

class HistoryWidget;

class HistoryModule : public DAVA::ClientModule
{
public:
    HistoryModule();

protected:
    void PostInit() override;

    // The main window closes before TArc tears the modules down. Detach the
    // widget from the command stack here, while the scene is still alive.
    void OnWindowClosed(const DAVA::WindowKey& key) override;

private:
    std::unique_ptr<DAVA::FieldBinder> binder;
    // TArc owns views registered with UI::AddView: UIManager::ModuleDestroyed
    // deletes them. Owning the widget here as well made it a double delete, and
    // that was the access violation inside QObject::~QObject on editor close.
    QPointer<HistoryWidget> widget;

    DAVA_VIRTUAL_REFLECTION(HistoryModule, DAVA::ClientModule);
};
